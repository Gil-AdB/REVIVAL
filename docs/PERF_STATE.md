# PERF_STATE.md — current state of the deferred pipeline (greets, 2026-05)

> **2026-08-28 — CONE ROUND 8 lands two BIT-EXACT wins on `cones-call` and
> builds two look flags that halve it again.** city t=1961, his arm
> `--env_live_water --deferred --city_env_pixel`, 1920×1080, interleaved
> min-of-11, box verified empty of other `DEMO` processes before and after
> every battery. `cones-call` **0.525 → 0.424 Gcyc/f and 15.32 → 13.38 ms
> with NO pixel moving anywhere** (`--cone_hull_rect` default ON +
> `anyLane_x8`, compile-time); **→ 0.099 Gcyc/f / 3.09 ms (−79.8 %) with
> `--cone_half_y_wide --cone_range_cull=0.5`**, both default OFF pending his
> eye. `renderFrame` 53.29 → 52.24 ms bit-exact, → 42.02 ms (−21.1 %) with
> both. 13/13 pins and `render_gate.sh` 4/4 on the shipped defaults. Full
> account, the two BOUND DEFECTS it found, the C3 refutation and four
> corrections to `docs/PERF_CONES_ANALYSIS.md`:
> `docs/OPTIMIZATION_BACKLOG.md` **2026-08-28**. Look deltas for his call:
> `docs/img/conesimpl/`.
>
> | arm | `cones-call` Gcyc/f | wall ms | Ginstr/f | `renderFrame` wall ms |
> |---|--:|--:|--:|--:|
> | round baseline | 0.525 | 15.32 | 2.081 | 54.81 |
> | + `anyLane_x8` (bit-exact) | 0.475 | 14.26 | 1.971 | — |
> | + `--cone_hull_rect` (bit-exact, **shipped**) | 0.424 | 13.38 | 1.838 | 52.24 |
> | + `--cone_half_y_wide` (OFF) | 0.229 | 6.92 | 0.946 | 45.74 |
> | + `--cone_range_cull=0.5` (OFF) | 0.099 | 3.09 | 0.412 | 42.02 |
>
> **`cones-call` was 30.9 % of `renderFrame` and the largest never-attacked
> row in the campaign. On the shipped defaults it is now ~25 %; with both look
> flags it is ~7 %.**

> **2026-08-28: §00l below is the CURRENT COST MAP for the three arms Gil-Ad
> actually runs**, and it supersedes §00 for those arms. **§00k and §00l are the
> same day and they overlap on ONE row**: §00k took city's `lighting-w1`
> −24.2 % *after* §00l's battery ran, so §00l's city column is a parent number
> and says so in its own banner. Everything else in §00l stands. §00 benched greets
> *flat* (no `--ssao`) and chase through the snapshot harness; both of his SSAO
> arms are new ground, `ssao` did not exist as a row in §00, and four defaults
> have moved since (`ssao_downscale` 1→2, `ssao_radius_zfloor` 0→48, the
> HDR/SSAO transport fix, `refl_correct` ON). §00's *method* and its per-row
> mechanism notes still stand and §00k cites them constantly. Read §00k for
> "what costs what today" and §00/§00a–j for "why".
>
> **2026-08-15: §00 rows 1 and 5 are amended in place by the omni-loop ladder**
> (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-15). Row 1's `Ginstr/f` is confirmed
> to 0.16 %; row 5's *share* is corrected upward and its attack is refuted.
>
> **§00 (2026-08-14) is the CURRENT baseline and supersedes §0's numbers.** §0
> (2026-08-08) is kept because its *ablation method* and its `--texture_filter`
> adjudication still stand, and because it is the anchor §00 measures drift
> against. §1–§2 and §9 are 2026-05 estimates, superseded twice over; read them
> for mechanism only.
>
> **§0 below (2026-08-08) supersedes the numbers in §1–§2 and §9.** Those were
> estimates and partial brackets from 2026-05, before `renderFrame` had any interior
> instrumentation and before the PBR/mip/env defaults moved. §0 is a measured,
> self-checking phase split of five poses across three scenes. The rest of the
> document remains the best description of the *mechanism* (kernel structure, cube
> tap, sampling modes, flag inventory) — read it for "how", read §0 for "how much".

---

## 00n. `Render_SSAO` DECOMPOSED — 2026-08-29: the row had ONE scope and an INFERRED interior; it now has five, the inference is CONFIRMED, and the march's per-lane slice setup goes 4-wide **bit-exact** for `ssao` −9.3 % (greets) / **−16.4 % (chase)**

§00l item 3 named the decomposition as "step one" and priced the interior by
arithmetic on two `--ssao_downscale` points. This section replaces that
arithmetic with scopes, and then attacks what they name. Branch `rev-ssao`;
gates 13/13 + `render_gate` 4/4 at every step.

### The instrument

`Render_SSAO` dispatched with `dispatchIndexed(..., nullptr, ...)` and joined
with a bare `tileDone.acquire()` loop, so it never used the `Stamp`/`drain`
pairing and printed no `effPar`. Five wave scopes now exist — `ssao-march`,
`ssao-hemi`, `ssao-blur`, `ssao-temporal`, `ssao-apply`. **The stamp must be
taken BEFORE the dispatch** (TailProf.h's own drain contract); a first attempt
that put the drain inside the lambda printed `0.00 calls/f`.

### MEASURED vs INFERRED — the inference was right

greets t=5743, 1920×1080, his arm:

| scope | **measured** | inferred (§00l) | effPar | `Ginstr/f` | **IPC** |
|---|--:|--:|--:|--:|--:|
| `ssao-march` | **5.712 ms** (73.3 %) | ≈5.8 | 11.0 | 0.621 | **3.26** |
| `ssao-apply` | **1.756 ms** (22.5 %) | ≈1.9 | 10.6 | 0.310 | **5.30** |
| `ssao-blur` | **0.272 ms** (3.5 %) | ≈0.33 | 9.0 | 0.032 | 3.79 |
| sum | 7.740 (99.3 % of the row) | | | 0.963 | |

Within 1.5 % on the march, 7.6 % on the apply, 17.6 % on the tiny blur, and the
scopes account for 99.3 % of the row — **there is no hidden fourth block.**
chase t=1105 has the same shape (march 73.8 %, apply 20.1 %, blur 5.2 %).

**What the inference could not give, and what chose the target:** `effPar`
9.0–11.0 of 12 workers — no serial bottleneck anywhere — and **IPC**. The apply
runs at **5.30**, near this core's ceiling, and is 32 % of the instructions but
only 22.5 % of the time. The march runs at **3.26**, is 64.5 % of the
instructions and 73.3 % of the time. The march is the one that stalls.

### Two candidates refuted before any code was written

* **The arm64 mask-lowering defect the cone round found does not exist here.**
  `DeferredSSAO.cpp` has **zero** `_mm256_movemask_*` sites, and the 32-sector
  visibility bitmask's eight scalar `__builtin_popcount` calls are **already
  vectorised by clang** into 2× `cnt.16b`.
* **Sky/background waste is not there either** (`-DFDS_SSAO_CENSUS`): ALL-SKY
  8-cell groups are **0.00 % in greets**, 5.08 % in chase; valid lanes 99.96 % /
  94.40 %; scalar-tail cells **0**. The march is dense, useful work.

### What landed — the slice setup, 4-wide and bit-exact

The per-lane slice setup (two cross products, a `fast_rsqrt`, a normal
projection, a third cross product, two dots and an `atan2_approx`, once per lane
per slice) is what §00l calls the one never-attempted structural item at
"~20 % of the compute". The new `-DFDS_SSAO_DIAG` ladder prices it at **22.5 %
of the march** (0.621 → 0.481 Gi/f), the atan2 alone at 7.6 % — **135
instructions per (lane × slice), 1.04 M a frame.**

| pose | `ssao-march` | | `ssao` | | `renderFrame` Gi |
|---|--:|--:|--:|--:|--:|
| greets t=5743 | 5.781 → **4.827 ms** (−16.5 %) | 0.620 → 0.528 Gi (−14.8 %) | 8.028 → **7.280 ms** (−9.3 %) | 0.962 → 0.870 (−9.6 %) | 5.588 → 5.496 (−1.65 %) |
| chase t=1105 | 8.394 → **6.547 ms** (−22.0 %) | 0.973 → 0.792 Gi (−18.6 %) | 11.279 → **9.432 ms** (−16.4 %) | 1.391 → 1.210 (−13.0 %) | – |

Predicted −0.09 to −0.105 Gi/f before measuring; **measured −0.092.**
`ssao-apply` (0.310 → 0.310 Gi) and `ssao-blur` (0.032 → 0.032) are byte-for-byte
unchanged — the control that proves the change is confined to the march.

### The bit-exactness, and how it was won

Written in **plain NEON, not simde**, so `fast_rsqrt`'s `vrsqrte` + one Newton
step is reproduced exactly rather than through an intrinsic simde may refine.
The first build failed ONE pin (greets t=2845), so `-DFDS_SSAO_VERIFY` runs the
scalar behind the vector and counts mismatches **per term**. It localised the
fault three times, and each time the answer came from compiling the scalar
expression standalone and reading the assembly — never from guessing:

1. `a*b - c*d` contracts to **one `fnmsub`**, not two muls and a sub.
2. For `A + B + C` all products, clang chains from the **second** term:
   `fma(C, fma(A, mul(B)))`. Starting at A moved **26 % of lanes**.
3. `atan2`'s final `a * poly` is **never materialised alone**: clang fuses it
   into `signedHalfPi - a*poly` (one `fmsub`) and `a*poly + (±π)` (one `fmadd`).
   Rounding it separately cost **32 196 lanes**.

Final: **0 mismatches in 1 036 800 lanes at t=2845 and t=5743**, on every term.
**This is a reusable rule set for vectorising any scalar float expression in this
tree, and it is the durable half of this round.**

**NOT TAKEN:** the existing `atan_approx_x8` uses `_mm256_rcp_ps` where the
scalar divides. It would be faster and it would move AO values — a look call in
the same family as the 8-wide GTAO rsqrt item already in Gil-Ad's stack
(backlog 2026-08-17a), not a perf lever.

---

## 00m. `lighting-w1` IN CITY, ROUND 2 (2026-08-28b): the pack loop's env fetch goes 8-wide — **−27.5 % instructions and −6.4 % of `renderFrame` cumulative** against the pre-campaign parent

Continuation of §00k; same kernel, same arm, same worktree. (Renumbered to
§00m 2026-08-29 — it was written as §00l on the same day the ranked cost map
landed with that letter, and two sections shared it for a day.) Full account:
`docs/OPTIMIZATION_BACKLOG.md` **2026-08-28b**.

**THE LADDER MOVED.** Round 1 shrank everything around the pack loop, so the pack
GREW as a share of the row — 24.8 % → **29.9 %** (0.221 of 0.739 Gi/f), second
only to the omni loop's 45.5 %. A new `-DFDS_OVEC_ENVDIAG=n` ladder splits it:
the two per-lane `EnvCubeFetchBil` calls are **5.1 % of the row** on their own,
the live-water tilt 3.4 %, the face pick 1.9 %, and 19.5 % is the rest of the
lane loop.

**THE CENSUS CHOSE THE SHAPE.** Of the groups carrying a vec-env lane,
**90.2–96.2 %** have every such lane on ONE cube face *after* the live-water
tilt, at 7.4–7.6 env lanes per group, and **100 % of lanes need both mip levels**
(2.00 fetches/lane, ~498 k a frame). Same-face implies same-mip 100 % of the
time, because gloss is per-material and 95 % of groups are material-uniform.

**LANDED: C9** (`EnvCubeFetchBil8` — the whole bilinear, eight lanes, one face,
one level; face pick and live-water tilt hoisted to a pre-pass so the uniformity
test can see all eight; scalar fallback for a mixed-face group) and **C10** (the
8-wide pack extended to env groups, which C7 never covered — it reproduces the
scalar's rounding ORDER: truncate each term, clamp the integer sum).

| pose | `lighting-w1` Gi/f | `Gcyc/f` | `renderFrame` Gi/f |
|---|---|---|---|
| city t=1961 | 0.978 → **0.709** (−27.5 %) | 0.245 → 0.180 (−26.5 %) | 4.184 → **3.915** (−6.4 %) |
| city t=400 | 0.733 → **0.508** (−30.7 %) | 0.185 → 0.127 (−31.4 %) | 3.174 → **2.949** (−7.1 %) |
| city t=2400 | 0.390 → **0.315** (−19.2 %) | 0.103 → 0.083 (−19.4 %) | 2.255 → **2.180** (−3.3 %) |
| fountain t=2500 | 0.105 → **0.090** (−14.3 %) | 0.029 → 0.025 | 1.061 → 1.045 (−1.5 %) |
| **greets (control)** | 1.478 → **1.476 (−0.14 %)** | 0.379 → 0.383 | 3.644 → 3.640 |

**REFUTED IN THIS ROUND, and §00k is corrected in place:** collapsing the four
round-1 dials to flagless is worth **−0.19 to −0.27 % of the row**, at the
±0.14 % Ginstr floor — not the 2.9 % §00k predicted. Clang had already hoisted
the loop-invariant bools; the disassembly loses exactly two `adrp`. The 2.9 % was
the OFF arm executing slow paths plus LTO layout.

**Gates:** 13/13 pinned poses + `render_gate` 4/4 after every step, byte-exact on
the first try.

---

## 00k. `lighting-w1` IN CITY — the OUTER-VEC kernel, first round ever run on it (2026-08-28): **−24.2 % instructions, −5.7 % of `renderFrame`**, and the row's shape is now measured, not estimated

Branch `rev-w1impl` off `fog-wt` `e017d611`. Arm
`--env_live_water --deferred --city_env_pixel`, 1512×848, `--bench=scene`,
`--deferred_prof=1 --hw_prof`. Full account, census and refutations:
`docs/OPTIMIZATION_BACKLOG.md` **2026-08-28**.

`Render_DeferredLighting_Tile_OuterVec` is a **different kernel** from the
`lighting-w1` this document's greets rows describe. greets and chase run
`Render_DeferredLighting_TileT` (1 px × 1 light, full shadow tap, GGX);
city / fountain / crash run OuterVec (8 px × 1 light, **no shadow tap of any
kind**, no GGX lobe — `--pbr` is structurally inert in it). Six of the eleven
landed greets `lighting-w1` levers are shadow-tap work and do not transfer.

**THE ROW'S SHAPE, measured by `-DFDS_OVEC_ABLATE` (this round's new ladder;
there was none before it).** City t=1961, row = 0.955 Gi/f:

| block | Gi/f | % of row |
|---|--:|--:|
| **the omni loop** | 0.476 | **49.8** |
| **the per-lane pack loop** | 0.237 | **24.8** |
| the per-lane material gather | 0.092 | 9.6 |
| the 8-wide env front-end | 0.037 | 3.9 |
| everything else (masks, decode, view pos, compose, store-outs) | 0.113 | 11.8 |

The two big blocks are the round's finding. The omni loop at ~50 % matched the
analysis's static estimate; **the pack loop at 24.8 % did not** (estimated ~5 %),
because **33–36 % of city's alive lanes carry an env store** and pay a scalar
env compose inside the pack. That block is untouched and is the next target.

**WHAT LANDED** — six byte-null levers (`--deferred_ovec_light_skip`,
`--deferred_ovec_mat_uniform`, `--deferred_ovec_nomirror`,
`--deferred_ovec_vec_pack`, plus two flagless), against the parent `e017d611`,
both binaries in one worktree, interleaved, min-of-5, Ginstr floor **±0.14 %**:

| pose | `lighting-w1` Gi/f | `Gcyc/f` | IPC | `renderFrame` Gi/f |
|---|---|---|---|---|
| city t=1961 | 0.978 → **0.741** (−24.2 %) | 0.244 → 0.187 (−23.4 %) | 4.01 → 3.96 | 4.183 → **3.945** (−5.7 %) |
| city t=2400 | 0.390 → **0.328** (−15.9 %) | 0.104 → 0.080 (−23.1 %) | 3.75 → 4.10 | 2.255 → **2.191** (−2.8 %) |
| city t=400 | 0.734 → **0.538** (−26.7 %) | 0.185 → 0.141 (−23.8 %) | 3.97 → 3.81 | 3.175 → **2.978** (−6.2 %) |
| fountain t=2500 | 0.105 → **0.088** (−16.2 %) | 0.029 → 0.025 | 3.62 → 3.52 | 1.060 → 1.043 (−1.6 %) |
| **greets t=5743 (control)** | 1.477 → **1.477 (0.00 %)** | 0.382 → 0.379 | 3.87 → 3.90 | 3.643 → 3.642 |

Cycles track instructions and IPC barely moves — not the cube-prepass pattern
where the two columns disagreed. greets is exactly flat, which proves the change
is confined to the OuterVec kernel.

> **ROUND 2 CORRECTION (§00l):** this section's closing recommendation — that a
> future round collapse the four dials to flagless for the last 2.9 % — is
> REFUTED. Measured at −0.19 to −0.27 % of the row, i.e. the Ginstr floor.

**C1 + C8 alone** (the two flagless levers, own binary, interleaved min-of-5):
t=1961 0.977 → 0.953 Gi/f (−2.5 %) and 0.243 → **0.231 Gcyc/f (−4.9 %)**; t=400
−3.3 % Gi / −3.2 % Gcyc. Cycles beating instructions is 16m's signature, and the
disassembly agrees: the OuterVec symbol goes from 2 `__cxa_guard` references,
13 `bl` calls and 10 callee-save `stp` pairs to **0, 6 and 9**.

**Gates:** 13/13 pinned poses + `render_gate` 4/4, and byte-nullity proved
DIFFERENTIALLY on one binary — each lever flipped off individually and all four
together reproduce the same hash at city t=1961, fountain t=2500, crash
t=400/1200, and greets t=5743 forced through `--deferred_outer_vec` (the only
arm that exercises the mirror-compare instantiation and the real normal-map lane
loop).

## 00l. 2026-08-28 — RANKED COST MAP AFTER THE SSAO/HDR ROUND. **§00 (2026-08-14) is superseded for the three arms Gil-Ad actually runs**: `ssao` did not exist as a row then and is now 14–25 % of two of the three arms, and chase's whole volumetric/AO/tonemap stack turns out to run **TWICE a frame**

> ### ⚠ THE CITY ROWS ARE ALREADY ONE ROUND STALE — read §00k above first
>
> Every number here was measured on **`e017d611`**, and §00k's OuterVec round
> landed on `fog-wt` the same day, **after** this battery ran. It takes city's
> `lighting-w1` **−24.2 % instructions** and city's `renderFrame` **−5.7 %** at
> t=1961. So this section's **city `lighting-w1` 11.924 ms, `renderFrame`
> 53.710 ms and tick 65.550 ms are PARENT numbers.** The city rows' RANKING is
> unaffected (`cones` 14.99 was already ahead of `lighting-w1` 11.92, and it
> widens), and greets / chase are untouched — §00k's own greets control is
> **exactly flat, 1.477 → 1.477 Gi/f**. Re-take the city column before quoting
> its absolute ms. §00k measured at 1512×848 and this section at 1920×1080, so
> the two are not directly subtractable.
>
> **One prediction in §00l.5 is CORRECTED by §00k and the correction is in
> place** — see item 2's city paragraph. OuterVec has **no shadow tap of any
> kind**, so "port the cube-tap wins to it" was the wrong mechanism; the
> measured next target is its **per-lane pack loop, 24.8 % of the row**.

**Read this section, not §00, for "what costs what today."** §00's *method* and its
per-row mechanism notes still stand; its numbers were taken before four defaults
moved (`ssao_downscale` 1→2, `ssao_radius_zfloor` 0→48, the HDR/SSAO transport
fix, `refl_correct` ON) and — more importantly — **§00 never measured Gil-Ad's
arms.** It benched greets *flat* (no `--ssao`) and chase through the snapshot
harness. Both of his SSAO arms are new ground.

Measured on `e017d611` (**fog-wt tip when this round started**; fog-wt has since
advanced to `0dc730d5`, see the banner) in a private worktree `/Users/gil-ad/work/rev-perfmap`,
branch `rev-perfmap`. One binary for every number in this section
(md5 `27efffb1cce906f1706b0b3286971797`), one asset tree, committed `rev.cfg`
(**1920×1080**, HiDPI 0), `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`,
`--profiler=0`, 12 pool workers. **AC power, battery 100 % charged, no thermal
or performance warning recorded by `pmset -g therm` before or after.**

> **RESOLUTION CAVEAT.** These are 1920×1080 numbers. Gil-Ad's working tree's
> `rev.cfg` currently says **1384×768** = 0.505× the pixels. Every per-pixel row
> below (`ssao`, `lighting-w1/w2`, `cones`, `fastfog`, `water-glints`,
> `tonemap-post`, `gbuf-clear`) roughly halves at his window; the geometry rows
> (`Tick-Xfrm`, `Tick-Radix`, `face-bin`, `shadow-bake`, `Tick-ReflXfrm`) do
> not. The RANKING is stable across that scale; the absolute ms are not.

### THE INSTRUMENTS THAT LANDED WITH THIS ROUND (both byte-null, both proved)

> **WHERE THEY LIVE.** Both instruments and all four battery drivers are on the
> side branch **`rev-perfmap`** (commit `b818bb2c`), NOT on `fog-wt` — only this
> document merged. They are byte-null and gate-verified against seven pins (see
> below), so merging them is a judgement call for the owner, not a risk; the
> numbers in this section cannot be reproduced on `fog-wt` without them.

1. **`--bench=scene@scene=chase,...` exists now** (`DEMO/Snapshot.cpp`,
   `RunSceneBench`). chase had no bench arm, so every prior round drove it
   through `--snapshot=chase@t=<t repeated N times>`, which interleaves a 6 MB
   `write_ppm` between ticks — usable for `[DPROF]` (those scopes live *inside*
   `tick()`) but useless as a **whole-tick clock**. chase now has the same
   `mean ms/iter` instrument greets and city have.
2. **city and chase tick work outside `renderFrame` is now attributed**
   (`DEMO/CITY.CPP`, `DEMO/CHASE.CPP`): depth-3 `TailProf` scopes
   `Tick-Clear` / `Tick-SkyCube` / `Tick-Anim` / `Tick-Light` / `Tick-ReflXfrm` /
   `Tick-Xfrm` / `Tick-Radix` / `Tick-ShadowBake`, mirroring the ones greets has
   had since 2026-08. **This cut the unaccounted residue from 10.9 % → 5.0 % of
   the city tick and 9.0 % → 3.3 % of chase's.**

**GATES on the instrumented binary** (it is the binary every number here was
taken on, so this matters): greets acceptance pin t=5743
**`440aa6bbb350ae95fbacf339dd2ad957`** reproduces; the five chase default pins
**`f16bedd0` / `fcc9d561` / `397b878d` / `3539492d` / `0622d56e`** all reproduce;
city `FDS_CITY_ENV_PIXEL=1` t=1961 **`bd4ffbf87d1492175a9b6c1111fb3f5f`**
reproduces. **The instrumentation is byte-null.**

> **METHOD SCAR, recorded so nobody loses an hour to it again:** the chase pin
> recipe is `--snapshot=chase@t=100,400,800,1200,1600` **as one process**.
> Running a SUBSET of those timestamps (I tried `t=100,800,1600`) gives
> different hashes at every pose but the first — chase's driver accumulates
> across ticks, so t=800 rendered after t=400 is not t=800 rendered after t=100.
> The five-value list is part of the pin, not a convenience.

### MEASUREMENT CONDITIONS AND NOISE FLOORS — read before quoting any number

Every table says min-of-N with the arms **interleaved and order-rotated every
round**, round 0 discarded. "floor" = `(2nd-min − min) / min`, the within-arm
spread of the estimator itself.

| battery | rounds | iters/run | 1-min load during | trust |
|---|--:|--:|---|---|
| **§00l.1–2 phase map** (5 arms) | 12, round 0 dropped | 24 | **3.9 → 9.5** | headline ms + splits |
| §00l.2 hardware counters | 6, round 0 dropped | 24 | 7.8 → 8.6 | `Ginstr/f`, `IPC` (load-robust) |
| §00l.4 flag ladders L1–L5 | 7, round 0 dropped | 24 | 8.7 → 13.7 | **deltas only** |
| §00l.4 GTAO ladder L6–L7 | 7, round 0 dropped | 24 | **17.7 → 27.2** | **deltas only, and coarsely** |
| between-binary floor | 7, round 0 dropped | 24 | 9.2 → 14.6 | the floor figure itself |

**The box was NOT quiet.** Two other agents were building and benchmarking on it
through most of this round; the load column is not decoration. What that buys
and costs:

* **The within-arm floors are still excellent** — 0.0–1.4 % on almost every
  headline row (`greets t=5743 ssao` +0.28 %, `chase t=800 cones` +0.12 %,
  `city cones` +0.05 %). The min-of-11 estimator is doing its job; a descheduled
  worker inflates the mean, not the minimum, and the minimum is what is printed.
* **Relative rankings and phase splits survive contention.** They are ratios
  inside one process, and the `Ginstr/f` column (load-robust by construction)
  agrees with them everywhere I checked.
* **Absolute ms were the part to distrust, and they were re-verified.** The
  whole 12-round battery was re-run at the end of the session under a
  **box-quiet protocol** — poll `pgrep` for builds and benches until the box is
  idle, then start; record load and battery before and after. It started at
  1-min load **2.89** with zero build/bench processes (and finished at 19.03,
  which is mostly the battery's own 12 workers). **The two batteries agree:**

| arm | tick, loaded battery | tick, quiet-start | Δ | `renderFrame` loaded | quiet-start | Δ |
|---|--:|--:|--:|--:|--:|--:|
| greets t=5743 | 56.536 | 56.590 | **+0.10 %** | 46.639 | 47.328 | +1.48 % |
| greets t=5965 | 48.090 | 48.553 | +0.96 % | 42.477 | 42.461 | −0.04 % |
| city t=1961 | 65.550 | 65.886 | +0.51 % | 53.710 | 53.976 | +0.50 % |
| chase t=1105 | 45.956 | 46.508 | +1.20 % | 42.291 | 42.757 | +1.10 % |
| chase t=800 | 55.536 | 55.725 | +0.34 % | 42.912 | 42.846 | −0.15 % |

  **Every whole-tick figure reproduces inside +1.2 %, and every headline phase
  row inside ±3.6 %.** Only two rows move more than 1.5 % between the batteries,
  and both are ones this section already calls out for their own reasons:
  `gbuffer` (+3.58 % greets, +3.55 % chase t=1105 — the load-sensitive raster
  phase) and greets' `RTT` (−2.66 %, a 1.7 ms dispatch row). `ssao` reproduces
  to **+0.16 / +0.03 / −0.07 %**, `cones` to **+0.22 / +0.24 %**, `fastfog` to
  **+0.27 %**, `lighting-w2` **exactly**. **The min-of-11 estimator was not
  materially contaminated**, which is what the tight within-arm floors already
  suggested. The tables in §00l.1–2 quote the LOADED battery (it has the same
  arms and the same round count); use the column above as the error bar.
* The ladder batteries (§00l.4) were **not** re-run quiet. They are same-binary
  flag flips read as deltas inside one interleaved batch, and their base arms
  agree across batches to 0.3 % (L1 base `ssao` 7.757 vs L6 base 7.737) — but
  quote them as deltas, not as absolute ms.
* Two rows carry a visibly worse floor and are flagged in place:
  `greets gbuffer` (+5.3 %) and `chase t=1105 gbuffer` (+5.9 % in the L2
  ladder) — the raster phase is the one that reacts to a busy box, which is
  itself consistent with it being the parallelism-bound phase (below).

**Between-binary floor.** The instrumented binary vs a control built from
pristine sources in the same worktree, same flags, same arms, 7 rounds
interleaved: greets tick **−0.45 %**, greets `renderFrame` **−0.43 %**, city tick
**+0.16 %**, city `renderFrame` **+0.86 %**, and on a single phase (`gbuffer`)
**±2.6 %**. So: **a cross-binary claim under ~1 % of frame is not a claim**, and
under ~3 % on one phase is not a claim. Same-binary flag flips (everything in
§00l.4) do not pay this floor and are quoted at their own floors.

---

### 00l.1 — THE THREE ARMS, WHOLE FRAME

His arms, verbatim:

```
greets  --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace
city    --env_live_water --deferred --city-env-pixel
chase   --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao
```

`--bench=scene@scene=<s>,t=<t>,iters=24 <arm> --profiler=0 --deferred_prof=1 --strict_flags`

| arm / pose | whole tick (ms/f) | floor | `renderFrame` | floor | `renderFrame` calls/f | outside `renderFrame`, attributed | UNACCOUNTED | unacc. % of tick |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| **greets t=5743** | **56.536** | +0.72 % | 46.639 | +1.39 % | 1 | 5.845 | 4.052 | 7.2 % |
| **greets t=5965** | **48.090** | +0.35 % | 42.477 | +0.08 % | 1 | 3.089 | 2.524 | 5.2 % |
| **city t=1961** | **65.550** | +0.58 % | 53.710 | +0.63 % | 2 (+1 off-view) | 8.566 | 3.274 | 5.0 % |
| **chase t=1105** | **45.956** | +0.81 % | 42.291 | +0.45 % | 2 (+1 off-view) | 2.160 | 1.505 | 3.3 % |
| **chase t=800** | **55.536** | +0.46 % | 42.912 | +0.42 % | 2 (+1 off-view) | 10.323 | 2.301 | 4.1 % |

**`renderFrame` calls/f is the single most load-bearing column in this table and
it is new.** city and chase each run **two main-view `renderFrame` calls a
frame** (the water-reflection pass and the main pass) plus one off-main-view
pass. `wall_min` for every row below is the **per-frame total over both calls** —
it is not a per-pass number. What differs between the two scenes is what the
reflection pass is allowed to skip, and that is §00l.5 item 1.

The remaining UNACCOUNTED residue (1.5–4.1 ms) is, in order of size:
the off-main-view `renderFrame` (`off wall` reads ≈0.74 ms chase, ≈0.82 ms city
in the same runs), `Flip`/`clearFrame` tails, `Dynamic_Camera`,
`Reflective_AnimateTexture`, `riskOfRain`, `driveBlasters`/`BlasterBolts_Draw`,
and greets' `ShadowBake_DispatchGreets` overlap window. **None of it is a single
big item**; I did not chase it below ~1 ms/row because nothing in it can be
larger than the residue.

---

### 00l.2 — THE PHASE SPLIT, PER POSE, RANKED

`wall_min` in ms per FRAME (both `renderFrame` calls where calls/f = 2).
`core-ms` = `thrsum_avg`, the Σ of tile-task time; `effPar` = core-ms / wall =
workers actually kept busy out of 12. `Ginstr/f` and `IPC` are from the
separate hardware-counter battery (box calibration: **IPC ≈2.42 compute-bound,
≈0.96 L1-latency, ≈0.14 L2-latency**).

#### greets t=5743 — tick 56.536, `renderFrame` 46.639

| row | depth | wall_min | floor | % of `renderFrame` | core-ms | effPar | Ginstr/f | IPC |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| `DeferredLighting-call` | 1 | **22.133** | +0.17 % | 47.5 % | – | – | 2.793 | 3.99 |
| ⤷ `lighting-w1` | 2 | **17.899** | +0.04 % | 38.4 % | 207.7 | 11.2 | 2.385 | 3.89 |
| ⤷ `lighting-w2` | 2 | 2.385 | +0.04 % | 5.1 % | 27.0 | 10.8 | 0.386 | 4.74 |
| ⤷ `mirror-grid` | 2 | 0.689 | +2.32 % | 1.5 % | – | – | – | – |
| ⤷ `depth-bounds` | 2 | 0.288 | +0.35 % | 0.6 % | – | – | – | – |
| `gbuffer` | 1 | **10.721** | +5.33 % | 23.0 % | 119.1 | 9.6 | 1.373 | 3.87 |
| **`ssao`** | 1 | **7.728** | +0.28 % | **16.6 %** | – | – | 0.962 | 3.73 |
| `shadow-bake` | 3 | 1.801 | +1.61 % | – | 13.3 | 6.7 | 0.135 | 3.28 |
| `bloom-chain` | 1 | 1.736 | +0.86 % | 3.7 % | – | – | 0.218 | 4.54 |
| `RTT` | 3 | 1.693 | +0.77 % | – | – | – | 0.020 | 2.38 |
| `cones-call` | 1 | 1.116 | +1.16 % | 2.4 % | 10.4 | 6.9 | 0.142 | 4.32 |
| `Tick-Light` | 3 | 0.809 | +0.37 % | – | – | – | – | – |
| `tonemap-post` | 1 | 0.668 | +0.30 % | 1.4 % | – | – | 0.060 | 2.67 |
| `Tick-Xfrm` | 3 | 0.448 | +1.56 % | – | – | – | – | – |
| `StampMasks` | 3 | 0.432 | +6.94 % | – | – | – | 0.003 | 1.95 |
| `TBR-render` | 1 | 0.408 | +0.25 % | 0.9 % | – | – | – | – |
| `gbuf-clear` | 1 | 0.333 | +2.40 % | 0.7 % | – | – | 0.005 | **0.58** |
| `Tick-Radix` | 3 | 0.293 | +0.68 % | – | – | – | – | – |
| `hdr-begin` | 1 | 0.259 | +0.00 % | 0.6 % | – | – | – | – |
| `xpar-peel` | 1 | 0.245 | +1.63 % | 0.5 % | – | – | – | – |
| `hdr-activate` | 1 | 0.187 | +1.60 % | 0.4 % | – | – | – | – |
| `OTHER (unattributed)` | 1 | 0.142 | – | 0.3 % | – | – | – | – |
| `shadow-uniformity` | 3 | 0.119 | +5.88 % | – | – | – | – | – |
| `GreetsCodeStage` | 3 | 0.115 | +0.00 % | – | – | – | – | – |
| `face-bin` | 1 | 0.110 | +2.73 % | 0.2 % | – | – | – | – |
| `Tick-Logic` | 3 | 0.105 | +0.95 % | – | – | – | – | – |
| `tile-cull` / `strip-lists` | 2 | 0.079 / 0.079 | – | 0.2 % ea | – | – | – | – |
| `sprite-insert` | 1 | 0.061 | +1.64 % | 0.1 % | – | – | – | – |
| `Tick-Anim` | 3 | 0.030 | +6.67 % | – | – | – | – | – |
| `light-list` / `w1-enqueue` | 2 | 0.020 / 0.016 | – | 0.0 % | – | – | – | – |

#### greets t=5965 — tick 48.090, `renderFrame` 42.477

The same frame minus the cones and the RTT. `ssao` does **not** shrink.

| row | wall_min | floor | % of `renderFrame` | Ginstr/f | IPC |
|---|--:|--:|--:|--:|--:|
| `DeferredLighting-call` | **20.374** | +0.32 % | 48.0 % | 2.523 | 3.94 |
| ⤷ `lighting-w1` | **16.149** | +0.07 % | 38.0 % | 2.114 | 3.82 |
| ⤷ `lighting-w2` | 2.384 | +0.13 % | 5.6 % | 0.388 | 4.79 |
| `gbuffer` | **9.953** | +0.02 % | 23.4 % | 1.238 | 3.96 |
| **`ssao`** | **7.702** | +0.05 % | **18.1 %** | 0.963 | 3.75 |
| `shadow-bake` (d3) | 1.852 | +0.16 % | – | 0.146 | 3.31 |
| `bloom-chain` | 1.762 | +0.91 % | 4.1 % | 0.218 | 4.47 |
| `mirror-grid` | 0.686 | +0.15 % | 1.6 % | – | – |
| `tonemap-post` | 0.667 | +0.15 % | 1.6 % | 0.060 | 2.67 |
| `StampMasks` (d3) | 0.459 | +1.09 % | – | – | – |
| `cones-call` | 0.375 | +0.00 % | 0.9 % | 0.037 | 4.16 |
| `Tick-Xfrm` (d3) | 0.345 | +1.16 % | – | – | – |
| `gbuf-clear` | 0.335 | +0.30 % | 0.8 % | – | **0.58** |
| `depth-bounds` | 0.271 | +1.85 % | 0.6 % | – | – |
| `RTT` (d3) | **0.001** | – | – | – | – |

#### city t=1961 — tick 65.550, `renderFrame` 53.710 (2 main passes)

| row | depth | wall_min | floor | % of `renderFrame` | core-ms | effPar | Ginstr/f | IPC |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| `cones-call` (1 call/f) | 1 | **15.022** | +0.11 % | 28.0 % | – | – | 2.082 | 3.99 |
| ⤷ `cones` | 2 | **14.990** | +0.05 % | 27.9 % | 176.5 | 11.3 | 2.082 | 3.99 |
| `DeferredLighting-call` | 1 | **13.023** | +0.34 % | 24.2 % | – | – | 1.586 | 3.99 |
| ⤷ `lighting-w1` | 2 | **11.924** | +0.37 % | 22.2 % | 132.2 | 10.6 | 1.578 | 4.01 |
| `fastfog` (1 call/f) | 1 | **8.977** | +0.55 % | 16.7 % | – | – | 1.070 | 3.60 |
| ⤷ `fog-composite` | 2 | 4.571 | +0.61 % | 8.5 % | – | – | 0.578 | 3.93 |
| ⤷ `fog-columns` | 2 | 3.635 | +0.14 % | 6.8 % | – | – | 0.400 | 3.17 |
| ⤷ `fog-glow` | 2 | 0.701 | +0.86 % | 1.3 % | – | – | – | – |
| `gbuffer` | 1 | **6.861** | +0.85 % | 12.8 % | 48.8 | **6.6** | 0.571 | 3.79 |
| `TBR-render` | 1 | **6.510** | +0.52 % | 12.1 % | – | – | 0.844 | 3.84 |
| `water-glints` | 3 | 2.554 | +0.31 % | – | 28.9 | 9.8 | 0.327 | 3.72 |
| **`Tick-ReflXfrm`** | 3 | **1.888** | +0.32 % | – | – | – | 0.018 | **2.53** |
| `water-ripple` | 3 | 1.776 | +0.23 % | – | 20.3 | 10.4 | 0.238 | 3.88 |
| `fog-skypaint` | 1 | 1.160 | +0.26 % | 2.2 % | – | – | 0.108 | 3.01 |
| `Tick-Light` | 3 | 0.919 | +0.00 % | – | – | – | 0.107 | 3.38 |
| `Tick-SkyCube` | 3 | 0.757 | +0.13 % | – | – | – | 0.067 | 3.42 |
| `gbuf-clear` | 1 | 0.681 | +0.15 % | 1.3 % | – | – | – | – |
| `depth-bounds` | 2 | 0.544 | +0.18 % | 1.0 % | – | – | – | – |
| `dispmap-resample` | 3 | 0.405 | +3.46 % | – | – | – | – | – |
| `strip-lists` | 2 | 0.279 | +0.00 % | 0.5 % | – | – | – | – |
| `xpar-peel` | 1 | 0.265 | +0.38 % | 0.5 % | – | – | – | – |
| `face-bin` | 1 | 0.169 | +5.92 % | 0.3 % | – | – | – | – |
| `Tick-Radix` | 3 | 0.165 | +0.00 % | – | – | – | – | – |
| `tile-cull` | 2 | 0.156 | +0.00 % | 0.3 % | – | – | – | – |
| `Tick-Clear` | 3 | 0.091 | +2.20 % | – | – | – | – | – |
| `Tick-Anim` | 3 | 0.011 | +0.00 % | – | – | – | – | – |
| **`ssao`** | 1 | **0.000** | – | – | – | – | – | – |

> **`ssao` is ZERO in city, and that is not a bug — his city arm has no
> `--ssao`.** The 2026-08-25 HDR/SSAO transport fix made city *able* to apply
> AO; it did not turn it on. **city pays nothing for SSAO today**, and any
> proposal to enable it there must be priced at roughly what greets pays
> (~7.7 ms at 1920×1080) before it is offered as a look upgrade.

#### chase t=1105 — tick 45.956, `renderFrame` 42.291 (2 main passes)

| row | depth | wall_min | floor | % of `renderFrame` | core-ms | effPar | Ginstr/f | IPC |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| `DeferredLighting-call` | 1 | **14.328** | +0.43 % | 33.9 % | – | – | 2.500 | **5.48** |
| ⤷ `lighting-w1` | 2 | **13.444** | +0.52 % | 31.8 % | 151.6 | 10.7 | 2.492 | **5.50** |
| **`ssao`** (2 calls/f) | 1 | **11.134** | +0.51 % | **26.3 %** | – | – | 1.391 | 3.73 |
| `gbuffer` | 1 | 5.204 | +0.04 % | 12.3 % | 48.0 | 7.3 | 0.587 | 4.00 |
| `cones-call` (2 calls/f) | 1 | **4.809** | +0.19 % | 11.4 % | 51.9 | 10.0 | 0.663 | 4.24 |
| `tonemap-post` (2/f) | 1 | 1.373 | +0.00 % | 3.2 % | – | – | 0.119 | 2.62 |
| `fog-legacy` (2/f) | 1 | 0.928 | +1.94 % | 2.2 % | – | – | 0.063 | 2.02 |
| `TBR-render` | 1 | 0.889 | +0.22 % | 2.1 % | – | – | 0.124 | 4.36 |
| `Tick-SkyCube` | 3 | 0.735 | +0.68 % | – | – | – | 0.066 | 3.49 |
| `gbuf-clear` (2/f) | 1 | 0.681 | +0.15 % | 1.6 % | – | – | 0.009 | **0.60** |
| `hdr-activate` (2/f) | 1 | 0.676 | +0.44 % | 1.6 % | – | – | 0.101 | 5.17 |
| `depth-bounds` | 2 | 0.536 | +3.92 % | 1.3 % | – | – | – | – |
| `Tick-Xfrm` | 3 | 0.397 | +1.51 % | – | – | – | 0.011 | 2.77 |
| `water-glints` | 3 | 0.397 | +0.50 % | – | 4.1 | 8.9 | – | – |
| `Tick-Radix` | 3 | 0.258 | +0.39 % | – | – | – | – | – |
| `hdr-begin` | 1 | 0.248 | +0.40 % | 0.6 % | – | – | – | – |
| `xpar-peel` | 1 | 0.207 | +0.97 % | 0.5 % | – | – | – | – |
| `Tick-ReflXfrm` | 3 | 0.192 | +0.52 % | – | – | – | – | – |
| `strip-lists` | 2 | 0.179 | +0.00 % | 0.4 % | – | – | – | – |
| `face-bin` | 1 | 0.152 | +2.63 % | 0.4 % | – | – | – | – |
| `Tick-Light` | 3 | 0.086 | +4.65 % | – | – | – | – | – |
| `Tick-Clear` | 3 | 0.083 | +0.00 % | – | – | – | – | – |

#### chase t=800 — tick 55.536, `renderFrame` 42.912 (2 main passes)

| row | depth | wall_min | floor | % of `renderFrame` | core-ms | effPar | Ginstr/f | IPC |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| `cones-call` (2 calls/f) | 1 | **13.623** | +0.10 % | 31.7 % | – | – | 1.979 | 4.31 |
| ⤷ `cones` | 2 | **13.558** | +0.12 % | 31.6 % | 154.6 | 10.6 | 1.979 | 4.31 |
| `gbuffer` | 1 | **8.585** | +1.60 % | 20.0 % | **30.0** | **3.1** | 0.400 | 4.07 |
| `water-glints` | 3 | **7.974** | +1.40 % | – | 91.1 | 10.6 | 0.962 | 3.51 |
| **`ssao`** (2 calls/f) | 1 | **7.485** | +0.00 % | 17.4 % | – | – | 0.895 | 3.66 |
| `DeferredLighting-call` | 1 | 4.646 | +0.86 % | 10.8 % | – | – | 0.576 | 5.38 |
| ⤷ `lighting-w1` | 2 | 3.651 | +0.14 % | 8.5 % | 33.9 | 8.5 | 0.569 | **5.50** |
| `TBR-render` | 1 | 2.555 | +0.04 % | 6.0 % | – | – | 0.459 | 5.34 |
| `tonemap-post` (2/f) | 1 | 1.352 | +0.96 % | 3.2 % | – | – | 0.119 | 2.63 |
| `hdr-activate` (2/f) | 1 | 0.999 | +0.10 % | 2.3 % | – | – | 0.168 | 5.88 |
| `fog-legacy` (2/f) | 1 | 0.915 | +1.64 % | 2.1 % | – | – | 0.063 | 1.97 |
| `Tick-SkyCube` | 3 | 0.728 | +0.14 % | – | – | – | 0.066 | 3.50 |
| `gbuf-clear` (2/f) | 1 | 0.676 | +2.07 % | 1.6 % | – | – | 0.009 | **0.59** |
| `depth-bounds` | 2 | 0.646 | +0.46 % | 1.5 % | – | – | – | – |
| `Tick-ReflXfrm` | 3 | 0.507 | +1.58 % | – | – | – | – | – |
| `Tick-Radix` | 3 | 0.398 | +0.00 % | – | – | – | – | – |
| `Tick-Xfrm` | 3 | 0.395 | +1.77 % | – | – | – | – | – |
| `xpar-peel` | 1 | 0.272 | +1.47 % | 0.6 % | – | – | – | – |
| `strip-lists` | 2 | 0.227 | +0.44 % | 0.5 % | – | – | – | – |
| `Tick-Light` | 3 | 0.222 | +0.00 % | – | – | – | – | – |
| `face-bin` | 1 | 0.180 | +2.78 % | 0.4 % | – | – | – | – |

**Every hot row reads IPC 3.5–5.5 against a 2.42 compute anchor — the pipeline
is compute-bound essentially everywhere.** The exceptions are exactly three and
they are all small: `gbuf-clear` (IPC 0.58–0.60, the one bandwidth-bound phase,
0.33–0.68 ms), `fog-legacy` (2.0), `tonemap-post` (2.6), and `Tick-ReflXfrm`
(2.53) / `Tick-Xfrm` (2.77) — the two per-vertex transform rows, which is what
"striding a 140-byte `Vertex`" looks like (§00g/§00h).

**The one parallelism outlier is `chase t=800 gbuffer`: effPar 3.1 of 12** —
30.0 core-ms spread over 8.585 ms of wall means **nine of twelve workers are
idle through that phase.** It is the only structural inefficiency in the map
that is not "the kernel does N things per pixel."

---

### 00l.3 — WHAT THE WHOLE-TICK CLOCK SEES THAT `[DPROF]` USED TO MISS

The brief for this round named `refl_correct`'s per-vertex normal (§00j) as the
known example. It is now a **row**, not a residue, in both scenes that pay it:

| scene / pose | `Tick-ReflXfrm` | IPC | what it is |
|---|--:|--:|---|
| city t=1961 | **1.888 ms** | 2.53 | `cityMirrorGlassForward()` + `Reflected_Transform(CitySc)`, `DEMO/CITY.CPP:3838` |
| chase t=800 | 0.507 ms | – | `Reflected_Transform(ChaseSc)`, `DEMO/CHASE.CPP:1484` |
| chase t=1105 | 0.192 ms | – | same |

§00j bracketed the whole `--refl_correct` effect at "≈2 ms/frame, quoted at one
significant figure, against a 2–7 % floor". **The direct scope agrees and
sharpens it**: city's entire mirrored-transform block is 1.888 ms with a
+0.32 % floor — so §00j's ~1.7 ms estimate for city was right, and it is now
measured rather than differenced. Note this row is the *whole* block (the
mirror transform exists with or without `--refl_correct`); the flag's marginal
cost is the TN/TTangent part of it, which §00j priced.

Full outside-`renderFrame` ledger, per arm:

| row | greets t=5743 | greets t=5965 | city t=1961 | chase t=1105 | chase t=800 |
|---|--:|--:|--:|--:|--:|
| `shadow-bake` | 1.801 | 1.852 | – | – | – |
| `RTT` | 1.693 | 0.001 | – | – | – |
| `water-glints` | – | – | 2.554 | 0.397 | **7.974** |
| `Tick-ReflXfrm` | – | – | **1.888** | 0.192 | 0.507 |
| `water-ripple` | – | – | 1.776 | – | – |
| `Tick-Light` (forward per-vertex `Lighting()`) | 0.809 | – | 0.919 | 0.086 | 0.222 |
| `Tick-SkyCube` | – | – | 0.757 | 0.735 | 0.728 |
| `Tick-Xfrm` (`Transform_Objects`) | 0.448 | 0.345 | – | 0.397 | 0.395 |
| `StampMasks` | 0.432 | 0.459 | – | – | – |
| `dispmap-resample` | – | – | 0.405 | – | – |
| `Tick-Radix` | 0.293 | 0.180 | 0.165 | 0.258 | 0.398 |
| `shadow-uniformity` | 0.119 | 0.124 | – | – | – |
| `GreetsCodeStage` | 0.115 | 0.114 | – | – | – |
| `Tick-Logic` / `Tick-Clear` | 0.105 | 0.009 | 0.091 | 0.083 | 0.087 |
| `Tick-Anim` | 0.030 | 0.029 | 0.011 | 0.012 | 0.012 |
| `Tick-ShadowBake` (city) | – | – | 0.000 | – | – |
| **Σ attributed** | **5.845** | **3.089** | **8.566** | **2.160** | **10.323** |
| **UNACCOUNTED** | 4.052 | 2.524 | 3.274 | 1.505 | 2.301 |

Two things in that table are worth saying out loud:

* **city's `Tick-Anim` is 0.011 ms and its `Tick-ShadowBake` is 0.000.** Neither
  is a target. `Animate_Objects` being 11 µs on a scene this size is a good
  result, not a measurement error — it was checked against chase (0.012) and
  greets (0.030).
* **`Tick-SkyCube` is 0.73–0.76 ms in city AND both chase poses**, and it is a
  full `RenderSkyCube` into VPage that the deferred path then paints over
  wherever geometry covers. Nobody has ever looked at it. 0.75 ms is 1.1–1.6 %
  of those ticks.

---

### 00l.4 — FLAG-FLIP LADDERS (same binary, so no between-binary floor is paid)

#### L1/L6 — greets t=5743: what the SSAO 7.7 ms actually is

| arm | tick | `renderFrame` | `ssao` | Δ`ssao` vs base |
|---|--:|--:|--:|--:|
| **base** (his arm) | 57.015 | 47.606 | **7.757** | – |
| `--no-ssao` | 48.810 | 39.574 | 0.000 | **−7.76** (frame −8.03) |
| `--ssao_blur=0` | 56.417 | 46.941 | 7.431 | −0.33 |
| `--ssao_samples=1` | 56.733 | 47.477 | 7.734 | **−0.02 (NOTHING)** |
| `--ssao_downscale=4` | 52.649 | 43.018 | 3.383 | −4.37 |
| `--ssao_downscale=3` * | 53.969 | 44.388 | 4.679 | −3.06 |
| `--ssao_gtao_slices=1` (from 2) * | 55.924 | 45.237 | 5.327 | **−2.41 (−31 %)** |
| `--ssao_gtao_steps=2` (from 4) * | 57.446 | 45.904 | 5.940 | **−1.80 (−23 %)** |
| `--ssao_gtao_slices=1 --ssao_gtao_steps=2` * | 55.382 | 44.160 | **4.418** | **−3.32 (−43 %)** |

\* the starred rows are the L6 ladder, run at load 17.7–27.2 — **deltas only,
and coarsely.** Its own `base` read 7.737 against L1's 7.757, which is the
reassurance that the arms are comparable inside each ladder. `--ssao_downscale=4`
was already measured by the 2026-08-16y round at **2.19 ms** (1512×848) and
**not taken — look cost max \|Δ\| 102–109**; my 3.383 at 1920×1080 is the same
number scaled, so that door stays closed. The `downscale` arms here are a
MEASURING INSTRUMENT for the interior split, not a proposal.

**Three findings, and one of them kills a knob:**

1. **`--ssao_samples` does nothing under `--ssao-gtao`.** 16 → 1 moved the pass
   by 0.02 ms against a +0.03 % floor. The GTAO producer
   (`FDS/RENDER/DeferredSSAO.cpp:352-354`) reads `ssao_gtao_slices` (default 2)
   and `ssao_gtao_steps` (default 4); `ssao_samples` feeds only the
   *hemisphere point-sampler* producer at line 627, which his arm does not run.
   **Anybody tuning greets/chase AO cost with `--ssao_samples` is turning a
   disconnected dial.**
2. **The pass splits, by the `downscale` slope, into a low-res part and a
   full-res part.** With `cost = A + B·lowN` and lowN ∝ 1/down²:
   down=2 → 7.757, down=4 → 3.383 ⇒ **B·N/4 ≈ 5.83 ms (the GTAO march, low-res)
   and A ≈ 1.93 ms (the full-res apply/upsample wave)**. The bilateral blur is
   0.33 ms of the march side. This is arithmetic on two measured points, not a
   scope — labelled INFERRED — but it is corroborated by the slices/steps rows,
   which move only the march.
3. **`--no-ssao` costs the frame 8.03 ms while the `ssao` row is 7.76** — the
   extra ~0.27 ms is downstream (the AO multiply's effect on what bloom and the
   tonemap then read). So the *realisable* prize on this row is the row plus
   ~3 %.

#### L2/L7 — chase t=1105: SSAO is a quarter of the frame

| arm | tick | `renderFrame` | `ssao` (2 calls/f) |
|---|--:|--:|--:|
| **base** (his arm) | 47.558 | 43.332 | **11.290** |
| `--no-ssao` | **35.637** | 31.340 | 0.000 |
| `--ssao_blur=0` | 47.734 | 42.728 | 10.892 |
| `--ssao_samples=1` | 47.890 | 43.355 | 11.445 |
| `--ssao_gtao_slices=1` * | 43.911 | 39.831 | 7.685 (−33 %) |
| `--ssao_gtao_steps=2` * | 44.068 | 40.244 | 8.937 (−22 %) |
| `--ssao_downscale=3` * | 42.376 | 38.465 | 6.670 (−42 %) |
| `--ssao_downscale=4` * | 40.522 | 35.625 | 4.799 (−58 %) |

**`--no-ssao` takes 11.92 ms off a 47.56 ms tick — 25.1 %.** That is the single
largest flag-attributable cost anywhere in his three arms.

#### L3 — chase t=800: the raster tile grid

| arm | tick | `renderFrame` | `gbuffer` | `cones` |
|---|--:|--:|--:|--:|
| **base** (6×5, the default) | 55.762 | 43.230 | **8.619** | 13.514 |
| **`--frame_tile_x=6 --frame_tile_y=20`** | **52.588** | **39.960** | **5.507** | 13.698 |
| `--frame_tile_x=12 --frame_tile_y=10` | 55.251 | 42.875 | 8.433 | 13.643 |
| `--no-ssao` | 47.709 | 35.439 | 8.752 | 13.629 |

**6×20 takes `gbuffer` −3.11 ms (−36 %), `renderFrame` −3.27 ms (−7.6 %) and the
tick −3.17 ms (−5.7 %), against a base floor of +0.01 %.** That is a
signal-to-floor ratio in the hundreds. **12×10 buys nothing (−0.19 ms)** — the
same 120 tiles spent on columns instead of rows. §00d said the shape matters and
this is the confirmation on chase.

**And on chase it is nearly byte-free.** Same 120-tile grid through the chase pin
recipe, all five poses:

| pose | px moved of 2 073 600 | % | max \|Δ\| | mean \|Δ\| on moved |
|---|--:|--:|--:|--:|
| t=100 | 57 | 0.00 % | 71 | 4.60 |
| t=800 | 327 | 0.02 % | 33 | 2.97 |
| t=1600 | 59 | 0.00 % | 52 | 3.03 |

(Hashes do move — `b67b47f0` / `5bc199d4` / `d1284b5a` / `9c0f7c2f` /
`9cdf5603` at 6×20 — so it is a re-pin, not a no-op. But 327 pixels is a
different order of magnitude from the 16 % §00d measured on greets.)

#### L4/L5 — the same grid on city and greets, and city's forward `Lighting()`

| city t=1961 arm | tick | `renderFrame` | `gbuffer` | `Tick-Light` |
|---|--:|--:|--:|--:|
| base | 65.639 | 53.910 | 6.863 | 0.919 |
| `--frame_tile_x=6 --frame_tile_y=20` | 64.850 | **53.055** | **5.961** | 0.919 |
| `--prof_no_vertex_light` | 64.745 | 53.829 | 6.786 | **0.000** |
| `--prof_no_fog` | 65.639 | 53.956 | 6.885 | 0.915 |

| greets t=5743 arm | tick | `renderFrame` | `gbuffer` |
|---|--:|--:|--:|
| base | 57.417 | **46.998** | **10.850** |
| `--frame_tile_x=6 --frame_tile_y=20` | 56.287 | 47.481 (**+0.48**) | 11.623 (**+0.77**) |
| `--frame_tile_x=12 --frame_tile_y=10` | 56.330 | 47.023 | 11.191 |

* **city gains from 6×20 too** (−0.90 ms `gbuffer`, −0.86 ms `renderFrame`,
  −0.79 ms tick), and it moves **4 534 px / 0.219 %, max \|Δ\| 160** at the pin
  pose — a real but small judge call. **Note this DISAGREES with §00d, which
  recorded city at 38 430 px = 1.85 %**; §00d measured a different arm and
  resolution, and I did not reconcile the two. Whoever takes the city half must
  re-measure the byte cost under the arm they intend to ship, not quote either
  figure.
* **greets LOSES from 6×20** (+0.48 ms `renderFrame`, +0.77 ms `gbuffer`) — so
  this must be a **per-scene default**, never a global one. (greets' tick column
  disagrees in sign with its own `renderFrame`/`gbuffer` columns; those two have
  the tighter floors and are the ones I believe. The tick column here is the
  loaded-box noise showing through.)
* **`--prof_no_fog` is inert on city's `fastfog`** — it gates the *legacy* fog
  pass, not `Render_DeferredFastFog`. Recorded so the next round does not use it
  as a fastfog ablation.
* **city's forward per-vertex `Lighting()` is 0.919 ms — 1.4 % of the tick.**
  If a backlog note ever put it at "~10 % of the frame on city under
  `--deferred`", **that is refuted at this pose**: `--prof_no_vertex_light`
  takes 0.89 ms off the tick and 0.919 off its own scope. It is not a target.

---

### 00l.5 — THE RANKED ATTACK LIST

Ranked by **absolute ms in the arm where the row is largest**; "total" is the row
summed across all five measured poses, as a second opinion on *breadth*. Every ms
is MEASURED; every "achievable" is labelled **MEASURED** or **INFERRED**. The
prior-attack column is a full sweep of `OPTIMIZATION_BACKLOG.md`,
`PERF_STATE.md`, `HW_PROFILING.md`, `SESSION_STATE.md`, `ARCHITECTURE_NEXT.md`,
`SOA_VERTEX_REFACTOR.md` and `fast_fog_*.md` done for this round —
**read (b) before writing any code, it is there so nothing gets re-refuted.**

---

#### 1. **chase's water-reflection pass runs the ENTIRE volumetric + AO + tonemap stack a second time.** **≈9.0 ms (t=1105) / ≈11.7 ms (t=800) upper bound — 19.6 % / 21.1 % of those ticks**

**(a) What the code does.** `DEMO/CHASE.CPP:1496` calls a bare `Render();` —
i.e. `renderFrame(RenderPath::Default, skipVolumetric = false)` — for the
mirrored water-reflection pass. `DEMO/CITY.CPP:3881` calls
`Render(RenderPath::Default, /*skipVolumetric=*/true)` for the *same* structural
pass, with the comment *"the pass-1 output is consumed by the dispMap
distortion, so volumetric work here is wasted CPU (≈60 ms/frame on the cube omni
halo)"*. `skipVolumetric` gates, in `FDS/RENDER/RENDER.CPP`: `Render_SSAO`
(`:730`), the SSAO debug write (`:741`), sky gradient (`:759`), the froxel
populate carve-out (`:779`), **HDR activation** (`:796`), glass refraction
(`:823`), and the whole cones / halos / rain / **tonemap** block at `:1380`.
The only other `skipVolumetric=true` sites in the tree are `EnvBake.cpp:481`,
`MirrorShatter.cpp:915` and `GREETS.CPP:4162`.

**This is why chase's `[DPROF]` shows `calls/f = 2` on `ssao`, `cones-call`,
`tonemap-post`, `hdr-activate` and `fog-legacy`, while city shows 1.**

**(b) Prior rounds.** **Proposed exactly once and never priced.** A 2026-08-17
handover in `OPTIMIZATION_BACKLOG.md:398` says, verbatim: *"chase never passes
`skipVolumetric`, so its reflected pass re-runs SSAO, bloom, tonemap, rain, DoF
and the froxel populate a second time. That is both a correctness hazard (fog
temporal history) and, **on its face, the largest single perf item left in
chase**."* **No A/B, no measurement, no refutation exists.** The same round
flagged two live correctness hazards from it: chase's SSR capture keys on
`skipVolumetric` (so it would misbehave if enabled) and chase defeats city's
`skipVolumetric` carve-out for the froxel fog's temporal history. **The numbers
below are the first pricing this item has ever had.**

**(c) Prediction.** Rows that would halve, from the measured per-frame totals:

| row (per-frame, both calls) | chase t=800 | chase t=1105 |
|---|--:|--:|
| `cones-call` | 13.623 | 4.809 |
| `ssao` | 7.485 | 11.134 |
| `tonemap-post` | 1.352 | 1.373 |
| `hdr-activate` | 0.999 | 0.676 |
| **half of the above** | **≈11.73** | **≈8.99** |
| as % of that pose's tick | **21.1 %** | **19.6 %** |

**Read that as an UPPER BOUND, not a promise.** Two measured reasons it is less:
the reflection pass covers less non-sky area than the main pass (SSAO early-outs
on `ze == 0`, `DeferredSSAO.cpp:384`); and **it is not byte-null and not even
look-null** — turning off `hdr-activate` there changes the reflection from an HDR
to an LDR underlay, and the 2026-08-17 round already noted chase's reflected pass
currently paints *a real (if incorrect) second unmirrored light shaft* that
Gil-Ad has not judged. **Offer it as a ladder, not a flip:** (i) skip only `ssao`
in the reflection pass — cheapest and least visible, worth ≈3.7 ms at t=800 and
≈5.6 ms at t=1105; (ii) + cones; (iii) full `skipVolumetric=true`.
**Step zero is one line and no risk: give the two passes distinct `TailProf`
names so the halving stops being an assumption.**

---

#### 2. `lighting-w1` — the deferred surface kernel's wave 1. **max 17.90 ms (greets t=5743) · total 63.1 ms** — the largest row in the map by total, and the most heavily refuted thing in the tree

**(a) What the code does.** `FDS/RENDER/DeferredSurfaceKernel.cpp:8158-8191`
(the `Stamp`/`drain` pair). greets/chase run `Render_DeferredLighting_Tile`;
city/fountain/crash run the `_OuterVec` monolith (`Scene::PreferOuterVec`,
`CITY.CPP:2586`). core-ms 207.7 at greets t=5743 over 12 workers, **effPar 11.2**
— the pool is saturated, there is no barrier tail to reclaim.

**(b) Prior rounds — ELEVEN of them, and the refusal list is long.** Wins taken:
packed shadow planes (27.620 → 26.423 ms), `--deferred_tile_sphere_cull`
(−3.3 % instr), the `computeMapShadowAtten` guard hoist (**−1.75 ms, −9.0 % of
w1; chase −22.5 %**), the **8×8 PolyId pyramid — already SHIPPED 2026-08-15b,
80.3 % of taps skip, −0.192 Gi/f (−6.5 %)**, `--deferred_cube_direct` (−7.9 %),
`--deferred_shade_ldr_skip`, `--deferred_cube_prepass` (Gcyc −13.8 %),
`ShadowSwzGetShape()`'s lazy static, `always_inline` on the spot tap.
**REFUTED with numbers — do not retry any of these:** the dynamic-plane skip
(**+12.4 % of w1's instructions**), passing world position in (+0.010 Gi), the
`noinline` 2×2 PCF (+0.51…+1.54 %), a runtime bool hatch (+4.3 % *with the flag
OFF*), the mirrorId group reject, the 4×4 pyramid (net −0.35 %, memory ×4), a
finer light tile grid past 12×8 (whole prize ≈0.012 Gi/f), the GGX hoist (LICM
already does it), the `Material` hot-record (≲0.1 ms). **The standing rule**
(`OPTIMIZATION_BACKLOG.md:4783`): *"the cube tap is at its register-allocation
limit and **any** new runtime predicate in its innermost body costs more than the
work it removes. Treat 'add a cheap test to the tap' as refuted-by-default; the
tap only gets cheaper by being CALLED LESS."*

**(c) Prediction.** **Do not re-attack the greets/chase kernel interior.** IPC
3.89 greets / **5.50 chase** — chase's is the highest of any hot row here.
**City's half of this row was taken while this battery was
running** — see §00k above. My draft predicted 0.8–1.5 ms on city *by porting
`--deferred_cube_direct` + `--deferred_cube_prepass` to the outer-vec path*, and
**that mechanism is wrong**: §00k establishes that `Tile_OuterVec` is a different
kernel with **no shadow tap of any kind** and no GGX lobe, so six of the eleven
landed greets levers are shadow-tap work that cannot transfer. What actually
landed there is six byte-null levers for **−24.2 % of city's `lighting-w1`
instructions / −5.7 % of `renderFrame`**, with greets exactly flat as the
control. **The measured next target on city is that round's own finding: the
per-lane pack loop at 24.8 % of the row** — five times the ~5 % its analysis
estimated, because 33–36 % of city's alive lanes carry an env store and pay a
scalar env compose inside the pack. Do not re-derive it here; §00k has the
ablation ladder (`-DFDS_OVEC_ABLATE`). Two other items are open and named. (i) The **only untried shape for the cube
tap itself** — make it **8-wide over PIXELS for a fixed light**, i.e. restructure
the loop pixel-major → light-major; *"large, and the per-pixel early-outs fight
it."* (ii) The `srcCube` mirror-reflection arithmetic on the spot tap (~40 float
ops × 828 k–1.77 M/frame, computed twice when both arms are live) — *"a real
lever and a different round"*. **Both must be quoted at t=3409 / t=3122, never at
t=5743**: the spot-tap call count spans 0 to 1.77 M/frame across six greets
poses, so a five-pose summary hides the sign.

---

#### 3. `ssao` — the whole SSAO stack. **max 11.13 ms (chase t=1105) · total 34.0 ms** · 26.3 % of chase's `renderFrame`, 16.6–18.1 % of greets'

**(a) What the code does.** `Render_SSAO()`, `FDS/RENDER/DeferredSSAO.cpp:212`,
called from `RENDER.CPP:730`. Under `--ssao-gtao` it runs **three fan-out/join
waves** over a static 12×8 = 96-tile grid (`:332`): the GTAO horizon march at
`:372` (low-res, W/2 × H/2 at `ssao_downscale=2`, `slices=2` × `steps=4` per
side), a bilateral denoise at `:794`, and a **full-resolution** depth-aware
upsample + apply at `:992`. The hemisphere point-sampler at `:627` and the
temporal wave at `:916` are dead in his arms.

**(b) Prior rounds — the pass HAS been attacked (2026-08-16, four rungs) but has
never been DECOMPOSED.** The 2026-08-16 round took `ssao` **2.143 → 1.652 Ginstr/f,
23.55 → 16.36 ms** at greets t=5743 via a slice-trig table + 8-wide denoise
(byte-null), a vector tail (byte-null), sector units (judge call) and an
`vld4_f16`/`vst4_f16` apply (byte-null) — image cost 343 px of 24 883 200.
2026-08-16y then flipped `ssao_downscale` 1→2, countersigned by Gil-Ad verbatim
(*"ssao downscale 2 is ok (no downscale looks much better, but too slow)"*),
cashing **14.55 → 4.96 ms**. **That dial is SPENT — do not re-propose it**, and
the same entry warns that the flip made the remaining SSAO items *"worth ~1/4 of
what this round priced them at."* `ssao_radius_zfloor=48` is a **look pin, not a
perf item** (|Δ| ≤ 0.08 ms over 14×14 interleaved runs; the old +0.17 ms figure
is retracted as noise). **DO NOT RE-PROPOSE, all tried and reverted:** the `cos⁴`
normal weight in denoise/upsample (bands); the geometric plane-distance weight
(0 gain, ~2× denoise cost); the **normal-aware upsample (+1.8 ms — edge-gating
deopts the apply loop's auto-vectorisation by ~1.2 ms)**; and **folding the apply
into the lighting kernel (full-rate kernel +5.4 ms against ~1.6 ms of apply
saved, net +3.8 ms — "keep the standalone apply")**. **Parked and explicitly not
attempted:** the per-lane scalar slice setup in `gtaoRow8` (cross products +
`fast_rsqrt` + `atan2_approx` per lane per slice, *"estimated ~20 % of the
compute, not attempted"*, bit-exact only if `atan2_approx`'s branches become
selects); and the two `_mm256_sqrt_ps` in `gtaoAcos_x8` (rsqrt-and-multiply is
**not byte-safe** — flips ~0.3 % of samples). **Open CORRECTNESS item riding
along:** 2026-08-17a found the 8-wide GTAO's two reciprocals disagree with its
own scalar reference on **up to 24 % of pixels** — *reported, not fixed*, fix is
one Newton–Raphson step, deferred because the file was contended.

**(c) Prediction.**
* **`--ssao_samples` is a dead dial under GTAO — MEASURED, 0.02 ms of 7.76.**
  It feeds only the hemisphere sampler at `:627`. Anyone tuning greets/chase AO
  cost with it is turning a disconnected knob; consider making it warn.
* **The pass has ONE profiler row and no `effPar` at all** (it uses
  `dispatchIndexed` without the `Stamp`/`drain` pairing, so `core-ms` and
  `effPar` print `-` — see the tables above). **Splitting it into
  march / blur / apply scopes is three lines and it is step one**, because the
  only per-wave number that exists in the whole tree is a doc note saying *"the
  apply's ~0.7 ms"* and my measurement disagrees with it.
* Interior split, **INFERRED** from the `downscale` slope (`cost = A + B·lowN`,
  `lowN ∝ 1/down²`; down=2 → 7.757, down=4 → 3.383): **GTAO march ≈ 5.8 ms,
  full-res apply ≈ 1.9 ms, blur ≈ 0.33 ms** at greets t=5743, 1920×1080.
  Corroborated by the slices/steps rows, which move only the march side. **This
  is arithmetic on two points, not a scope — replace it with the scopes.**
* The march is very nearly linear in `slices × steps` (`slices=1` −31 %,
  `steps=2` −23 %), which means **there is no fat in the loop structure — inside
  it, only fewer samples helps, and that is a quality call.** The one named,
  never-attempted structural item is the per-lane slice setup above (~20 %).
  **INFERRED 0.8–1.2 ms on greets if it lands, and it is a judge call on bytes.**

---

#### 4. `cones` — the volumetric cone pass. **max 14.99 ms (city t=1961) / 13.56 ms (chase t=800) · total 34.7 ms**

**(a) What the code does.** `Render_VolumetricCones_Tile`,
`FDS/RENDER/DeferredVolumetric.cpp:776`, dispatched at `:3113`/`:3129`, scoped at
`:3123`. Nest is row → 8-px batch → spot. city runs it once per frame, chase
twice (item 1). core-ms 176.5 / effPar 11.3 in city — saturated pool.

**(b) Prior rounds — SEVEN, and the do-not-retry list is the longest in the
tree.** Taken: R1 `--vol_cone_solve_vec` (city 30.764 → 21.406 ms, −30.4 %);
R2 `--vol_cone_lane_vec` (→ 17.145, −19.8 %); R4 `FDS_CONE_NEONMINMAX`;
R5 `FDS_CONE_NEONSTEP`; R6 un-gating the vector solve on the segmented path +
`FDS_CONE_QUADEARLYOUT` (chase 18.304 → 14.930, **−18.4 %**); R7 the per-tile
`ConeSpotPre` hoist (chase −9.1 % instr, city −8.1 %, **bit-exact**). **R6 is the
"wrong scene" lesson**: rounds 1–5 measured city, where **0.0 %** of pairs are
segmented, while chase is 90–100 % segmented — every city-derived cone
conclusion is an all-wide-branch result. **Do not retry:** the range-sphere
early-out (+2.0 %), raw rcp/rsqrt (+1.6 %), finer cone tiles, the `1/uV` hoist
(*killed twice, LICM already does it*), per-lane noise loop, per-spot broadcasts,
W4 rolled (+7.5 % cycles), the `zLo/zHi` select collapse, the 8-segment closed
form (+0.1…+0.7 % instr), register-handover fusion (+2.0 %). Culling is capped at
**~3 %**, unroll-and-jam at **~8 %**. §11's rule: *"stop counting instructions on
this pass"* — `FDS_CONE_HOTONLY` deletes 8–10 % of instructions for **zero cycles
and zero wall**. **And the standing warning:** *"do not price a cull on this pass
without its fire rate first"* — 83.9 % / 18.8 % / 3.4 % across three poses, and
**the verdict flips sign across that range.** Left unpriced: the per-spot
shadow-map block behind `spotAlive`; greets' `shadowed=51` per-segment tap.

**(c) Prediction.** On chase, **the biggest cone win available is item 1** — stop
running the pass twice — not anything in the kernel. On city, §00's ceiling holds:
**INFERRED 0.5–1.2 ms** from the two unpriced blocks. A separate agent is
auditing the city cones path this session; take its findings before spending a
round here.

---

#### 5. `gbuffer` — the raster / G-buffer fill. **max 10.72 ms (greets t=5743) · total 41.3 ms** · at chase t=800 it is the ONLY parallelism-bound row in the map, and **the fix is already written, measured, and un-taken**

**(a) What the code does.** `RENDER.CPP:660-680`; the tile grid is
`--frame_tile_x` × `--frame_tile_y`, default **6×5 = 30 tiles**, read at
`RENDER.CPP:469-470` and rounded down to a multiple of 8. At chase t=800:
30.0 core-ms over 8.585 ms wall = **effPar 3.1 of 12 — nine workers idle.**

**(b) Prior rounds.** §00 row 9 (2026-08-14) refuted a finer grid at *"+41 %
wall, +139 % instructions"* because each clipper tile re-walked the whole face
list. **That refutation is DEAD**: 2026-08-16c removed the traversal
(`c26c1c35` + `d9dfa527` `--face_tile_bin`) and the identical 12×10 probe then
cost **+2.7 %** — §00d says in terms *"nobody should quote row 9 as a reason
again."* §00d also established that **shape dominates count**: at a fixed 120
tiles, `6×20` beats `12×10` by 35 % of the gbuffer wall and `24×5` by 33 %; at
chase t=800 it took `gbuffer` **8.50 → 5.22 ms (−38.5 %)**, effPar **2.9 → 5.1**,
`renderFrame` **−11.7 %** for +0.5 % instructions. The 16→20 row cliff is
**recorded as an unexplained open question.** The flags landed at the historical
6×5 default, byte-null, and **nothing was switched on.** **The per-scene default
WAS proposed and never taken**, verbatim: *"If the chase pins are acceptable at
that scale, `FF::setDefault(frame_tile_y, 20)` in `CHASE.CPP` is the whole
change. **Per-scene, not global.**"* / *"**greets: no.** Flat wall, 16 % of
pixels moved."* / *"**city: marginal.**"* I confirmed **no `setDefault(frame_tile_*)`
exists anywhere in `DEMO/` today.**

**(c) Prediction — MEASURED this round, and it is the cheapest real win in the
map.**

| scene | Δ`gbuffer` at 6×20 | Δ`renderFrame` | Δ tick | pixels moved (this round) |
|---|--:|--:|--:|---|
| **chase t=800** | **−3.11 ms (−36 %)** | **−3.27 ms (−7.6 %)** | **−3.17 ms (−5.7 %)** | **327 of 2 073 600 (0.02 %)**, max \|Δ\| 33 |
| **city t=1961** | −0.90 ms | −0.86 ms | −0.79 ms | 4 534 (0.219 %), max \|Δ\| 160 |
| **greets t=5743** | **+0.77 (worse)** | **+0.48 (worse)** | – | §00d: 339 472 px = 16.37 % |

12×10 buys **−0.19 ms** — the same 120 tiles spent on columns instead of rows.
**Mechanism (now certain):** chase's raster is row-bound — its geometry is a wide
horizon band, so with 5 row-tiles a few workers own almost all the faces.
My byte counts (57 / 327 / 59 px at t=100/800/1600) independently reproduce
§00d's *"chase 58–1 738 px"*. **Land it as a per-scene default: chase 6×20
certainly; city 6×20 only after Gil-Ad looks at 4 534 px; greets stays 6×5.**
Cost: one `setDefault` line plus a five-hash re-pin
(`b67b47f0`/`5bc199d4`/`d1284b5a`/`9c0f7c2f`/`9cdf5603`), and a memory note
recorded by the `--face_tile_bin` round: its arena grows **403 KiB → 851 KiB at
6×20** (1.22 MiB at 24×20). Not a blocker; state it in the commit.

greets' 10.72 ms `gbuffer` is a **geometry** row (displaced stone), effPar 9.6,
already priced by §00's displacement family table. No new lever found.

---

#### 6. `fastfog` — city's froxel fog. **8.98 ms, city only, 16.7 % of its `renderFrame`**

**(a) What the code does.** `RENDER.CPP:780` → `Render_DeferredFastFog(dctx)`.
The two sub-scopes that carry it: **`fog-composite` 4.571 ms**
(`DeferredFastFog.cpp:3034`) and **`fog-columns` 3.635 ms** (`:3026`), plus
`fog-glow` 0.701 and the separate `fog-skypaint` 1.160. `fog-columns` reads
IPC 3.17 — the lowest of the hot rows.

**(b) Prior rounds.** §00 row 8 per-symbol split: `FastFog_SampleGrid` 4.3 %,
`Froxel_CompositePixel` 4.1 %, `vFogNoise` 3.9 %, the three tile lambdas 5.9 %,
`SkyPaint` 1.8 %, `vBlobNoise` 0.7 % of city's whole CPU — *"the noise is the
cost."* 2026-08-16b cached `Froxel_CompositePixel`'s per-pixel
`1/log(far/near)` per frame (`fog-composite` −3.2…−3.8 %, bit-exact). 2026-08-16z
was the only dedicated round: `--fog_composite_tile_align8` taken (−2.93 % of the
sub-row) with the honest caveat that *"the frame does not resolve this"*.
**DO NOT REOPEN** (`OPTIMIZATION_BACKLOG.md:648`): the glow `atanf` (deleting it
*entirely* is `renderFrame` −0.79 %; a polynomial measured −0.24 %, under the bar),
the live-water slope indirection (0.10 %), lane-level composite punting (**87.7 %
of punted groups have all eight lanes reflective → ≤6 % recoverable**). Also
arm64-negative and parked for x64: the SIMD 8-corner hash (−1 to −2 ms
*regression*), corner caching, packed AoS froxel record, vectorised `fastExpNeg`.
**Note for the next agent: `--prof_no_fog` does NOT gate this pass** (measured
inert this round — it gates the *legacy* fog).

**(c) Prediction.** Two named items survive. **The composite's remaining scalar
half — 335 184 punted px/frame, ≈0.030 Gi/f — is the biggest remaining fastfog
item**, and the specified shape is *"the water-reflection branch written 8-wide,
scalar lanes first, vector store masked."* **INFERRED 0.3–0.6 ms.** Separately,
**`FastFog_SampleGrid` (4.3 % of city's CPU) has NO prior attack at all** — it is
named in the census and nowhere else. **INFERRED unknown; census it first.**
`fog-columns` at 0.400 Gi/f is the noise and *"no new lever found"* — leave it.

---

#### 7. `water-glints` — chase's procedural sea. **7.97 ms at chase t=800 (18.6 % of its `renderFrame`), 2.55 ms city** · **and the win city already took has NEVER reached chase**

**(a) What the code does.** chase takes `pwater::RenderGlintsVaried`
(`DEMO/ProceduralWater.cpp:792`, banded at `:884`); city takes the plain
`RenderGlints` (`:623`, banded at `:785`). Outside `renderFrame`. effPar 10.6,
IPC 3.51, 0.962 Ginstr/f at chase t=800.

**(b) Prior rounds — four, and the last one bypassed chase by construction.**
2026-08-15d: `runRowBands` 8-row atomic chunks + an `ndhMin` skip ahead of `powf`
— **chase 14.195 → 10.021 ms (−29.4 %)**, city 7.720 → 4.602, bit-exact.
2026-08-15e: the caustic sum-plane — chase 11.190 → 9.297 (−16.9 %), a judge call
at 7 px. 2026-08-16b: the `% WNRM` → 129-stride halo (161 → 132 instructions).
**2026-08-16e: `--water_slope_vec8`, BYTE-NULL — city `water-glints` Gcyc/f
0.082 → 0.054 (−34 %), wall 2.423 → 1.590 ms.** And then, verbatim
(`PERF_STATE.md:1615`): *"**Chase and greets are untouched by construction as
well as by pin — chase's water goes through `RenderGlintsVaried` /
`waterWaveSlopeVaried`, a separate copy this round does not open.**"*
**Do not retry:** constant texture dims / removing 5 of 9 `fdiv` (instructions
went *up*), the occlusion-test hoist (1.4 %), the per-row horizon early-out
(0.7 %). And the standing correction: *"THE FOUNTAIN-198M PATTERN IS NOT WHAT
THIS IS"* — the reject path is ~10 instructions against ~1050 per live pixel.

**(c) Prediction — this is the best-shaped un-taken win after items 1 and 5.**
**Port `--water_slope_vec8` to `waterWaveSlopeVaried`.** It is byte-null on the
city copy, the mechanism is identical, and city measured **−34 % of the row's
cycles**. **INFERRED 1.5–2.5 ms at chase t=800** (a −34 % cycle result on a
7.97 ms row, discounted because the `Varied` copy has extra terms). Then the
second, already-priced item: *"five libm calls per live pixel"* — the four swell
transcendentals ablate at **−20.1 % instructions / −11.7 % wall of the chase
pass** and the lever is a 4-wide vector `cos`, a judge call on ~1 ulp
(**INFERRED further 0.9 ms**). Note the residue is *"the `powf` lobe and the
caustic tap, not the slope"* once the vec8 lands.

---

#### 8. `TBR-render` — the transparent/sprite lighting pass. **6.51 ms city, 2.56 ms chase t=800, 5.00 ms chase t=1600 (per §00)**

**(a) What the code does.** `RENDER.CPP:1360` → `TBR_Render(CurScene, &dctx)`;
the kernel is `Render_DeferredTransparentLighting_Tile<0>` (front-facing) / `<1>`
(back-facing). IPC 3.84 city / 5.34 chase.

**(b) Prior rounds — and the obvious idea is already dead.** The **fountain**
instance was §00 row 3 and CLOSED 2026-08-14b at **−11.99 ms of a 27.46 ms frame
(−43.7 %)**: the per-strip dispatch bounded Y and only Y, so 12 884 composite
invocations a frame scanned the full 1920-px strip width (197.90 M px scanned for
0.97 M live — 0.491 %) and peel passes 1–3 were 100 % empty. **That fix WAS
measured on city and chase in the same table and it is NULL there**
(`OPTIMIZATION_BACKLOG.md:4941-4947`): city t=1961 `TBR-render` **7.00 → 6.77**,
chase t=1600 **4.61 → 4.62**, verdict *"the other three scenes are NULL, as
expected — they do not run thousands of sprite-delimited clumps a frame. **This
is a fountain fix.**"* Mechanism for why: city/chase run **one** peel pass, their
`.xparPeel = 4` never engages, and forcing it is byte-identical (*"DO NOT RUSH TO
WIRE IT"*). **But city's and chase's TBR rows have never been censused or
attacked in their own right** — `--xpar_extent_census` has only ever been run on
fountain t=1200, the §00b ranked table's attack cell for city's TBR is empty, and
no "do not re-propose" warning exists for them.

**(c) Prediction.** Do **not** re-run the fountain diagnosis; it is excluded.
Do run `--xpar_extent_census` on city t=1961 once — it is one run and it is the
only instrument that exists — to establish what city's 6.51 ms actually *is*
before anyone designs for it. **INFERRED 0–2 ms, genuinely bimodal**, and the
honest statement today is **"unopened", not "attackable"**.

---

#### 9. greets' three ~1.7 ms passengers: `shadow-bake` 1.80 + `RTT` 1.69 + `bloom-chain` 1.74 = **5.23 ms of a 56.5 ms tick (9.3 %)**

**(a)** `shadow-bake` — `Render_DeferredShadowMaps`, outside `renderFrame`,
**effPar 6.7 of 12** (the second-worst parallelism in the map). `RTT` — the
mirror offscreen pass, **`Ginstr/f` 0.020 and IPC 2.38**: almost no work, 1.69 ms
of wall, and **0.001 ms at greets t=5965** where no RTT job ran. `bloom-chain` —
`RENDER.CPP:1411`, IPC 4.54.

**(b) Prior rounds.** `shadow-bake`: 2026-08-16q's `--shadow_bbox_cull` (default
ON) took clipper entries **231 735 → 41 787 (−82.0 %)** and `DynMeshes` raster
−40.0 %, byte-exact over 79.7 M face-visits. §00 calls the row **"mature"**, and
two doors are explicitly closed — shadow-map tiling (*"linear won all 15
shape-measurements. Closed."*) and shadow-bake ∥ gbuffer overlap (*"IMPLEMENTED,
GATED OFF, MEASURED NET-NEGATIVE — do not default on"*, p50 40.0 → 52.5 ms).
**Two items are open and named:** (1) *"`FDS_SHADOW_TILE_GRID` is now the wrong
shape and nobody has re-asked it… a per-light FACE→TILE BIN — i.e.
`--face_tile_bin` for the shadow pass — would collapse the 237 609 pair-visits a
frame to two list walks; `FaceTileBin.cpp` already exists and its
order-preservation proof already covers this shape. **Not built.**"* (2)
**`DynOmnis` is now the bigger bake (1.21 ms raster vs `DynMeshes`' 0.78) — 28
maps of which 18 are single-tile 128², untouched, and the cost is per-map fixed
overhead, not clipper population.** `bloom-chain` / `tonemap-post` /
`hdr-activate`: **the entire 2026-08 campaign never touched them.** All prior work
is 2026-07 — the f16 HDR buffer (frame min 50.7 → 43.6 ms) and half-res DoF
(−3 ms) — and the one pricing note reads *"long tail — no hidden monster."*
Do-not-re-propose: pointwise post-FX fusion (*"only pays with chromatic off
(~0.3 ms); not built"*). `RTT`: §00 row 13 — *"expected, priced, not a defect."*

**(c) Prediction.** **`RTT` is the odd one and the best-shaped of the three:
0.020 Ginstr/f producing 1.69 ms of wall at IPC 2.38 is not a compute row** — it
is dispatch, allocation or wait, and the same row is 0.001 ms one pose over.
**INFERRED 0.5–1.0 ms** by making the per-frame RTT conditional on the mirror
being visible-and-changed; the predicate machinery already exists
(`fds::g_rttJobsLastFrame`, used at `GREETS.CPP:4078`). `shadow-bake`'s two named
open items (the shadow-pass face-tile bin, and `DynOmnis`' 18 single-tile 128²
maps): **INFERRED 0.4–0.7 ms**. `bloom-chain` at IPC 4.54, unattacked since 2026-07:
**INFERRED 0.3–0.6 ms, no mechanism identified** — a scoping run first.

---

#### 10. `lighting-w2` — the checkerboard fill wave. **2.39 ms greets, both poses, dead flat**

**(a)** `DeferredSurfaceKernel.cpp:8207-8214`. IPC **4.74–4.79** — the highest in
greets. effPar 10.8.

**(b) Prior rounds — two wins and a closing statement.** 2026-08-16h
`--deferred_fill_ldr_skip` + a material hoist: **w2 −7.0…−7.4 % at every pose**;
the same round's census **refuted its own hypothesis** — the full-shade edge
fallback is **768 of 641 088 cells = 0.12 %**, so *"no attack on the edge
classification is justified on cost grounds."* 2026-08-16o: 4-wide
`oct_decode_u32_x4`, **0.272 → 0.239 Gi/f (−11.9…−13.4 %)**, bit-exact over
57.3 M lanes. **Do not re-propose:** turning the checkerboard OFF (**53.1 →
79.3 ms**); the scanline carry for the neighbour gather (**tried twice, both net
zero**, +23 %/+20 %/+8.2 %); the skip-when-equal oct fast path (dissolved by the
4-wide). The durable rule: *"in this kernel a flag-guarded predicate or eight
extra live values in the pixel body cost about what any of these mechanisms
save."* **And the closing line, verbatim:** the 3-channel scalar arithmetic is
*"the only one big enough to matter"* at ~0.03 Gi/f but *"a 4-wide rewrite is NOT
bit-exact by construction — the `fdiv` reassociation is where it will break"*,
after which *"**nothing else in this row is worth a round.**"*

**(c) Prediction. INFERRED 0.2–0.4 ms and a judge call on bytes.** This row is
close to closed and the docs say so. **Take it last, if at all.**

---

#### Also on the board, below the top ten but measured and named

| row | ms | note |
|---|--:|---|
| **`mirror-grid`** (greets) | **0.689** | **The cleanest un-refuted, parallelism-shaped row in the tree.** §00 row 14: *"still unattacked since the backlog flagged it"*; flagged three separate times since 2026-07-03 (*"still serial, parallelizable follow-up"*), achievable 0.3–0.5 ms. A full-res scalar scan of the mirrorMask plane every frame (`DeferredSurfaceKernel.cpp:7932`). The adjacent, already-identified item: `mirrorMask` (u8) and `mirrorMaskZ` (u16) are 3 bytes across **two allocations 2 MB apart**, read together twice per rasterizer row — the backlog's *"second-cleanest"* hot-struct merge candidate. **INFERRED 0.3–0.5 ms, low risk.** |
| `Tick-ReflXfrm` (city) | **1.888** | IPC 2.53. §00j priced `--refl_correct`'s marginal half at ~1.7 ms against a *larger* between-binary floor; this scope measures the whole block directly at a +0.32 % floor. **Not an overhead target** — §00j: *"that work IS the feature."* The structural answer (SoA Phase 5) is **NO-GO / BLOCKED ON SCOPE**: 274 refs in DEMO scene code including three alternative transform pipelines, end state measured at 0.56 % of a greets frame, and *"there is no bit-exact subset of Phase 5 that pays."* |
| `Tick-SkyCube` | **0.73–0.76 in ALL THREE arms** | A full `RenderSkyCube` into VPage that the deferred path then paints over wherever geometry covers. **Zero prior history found.** 1.1–1.6 % of three ticks for mostly-overdrawn pixels; `--deferred_skybox` (`RENDER.CPP:749`) already exists as the replacement and is default-off *"until visually validated"*. **INFERRED 0.4–0.7 ms × 3 scenes; needs Gil-Ad's eye, not a measurement.** |
| `tonemap-post` | 1.35–1.37 chase, 0.67 greets | IPC 2.6, 2 calls/f in chase (item 1). Untouched since 2026-07. |
| `water-ripple` (city) | 1.776 | effPar 10.4; cashed 2026-08-15d alongside the glints. |
| `Tick-Light` (city forward `Lighting()`) | **0.919** | **This CONFIRMS a fix, it does not refute a claim.** The *"~10 % of the city frame"* figure (`PERF_STATE.md:1491`) is the **pre-fix** number — 2026-08-16b diagnosed it as a load-imbalance row (`effPar` ≈ 1.5, the fan's unit was a MESH and one mesh is the whole building), chunked it to 1 024-vertex ranges byte-null, and took `LGHT` p50 **5.904 → 0.974 ms** (frame min −4.96 ms). My 0.919 matches the post-fix 0.974 within noise. **Anyone quoting "10 % of the city frame" today is quoting a dead number.** Not a target. |
| `gbuf-clear` | 0.33–0.68 | **IPC 0.58–0.60 — the only bandwidth-bound phase in the tree**, and deliberately unattacked: it is the instrument's own control, proving IPC discriminates rather than reading 3.9 everywhere. |
| `face-bin` | 0.11–0.18 | Already attacked and landed (`--face_tile_bin`, 2026-08-16c, byte-null). Build cost 0.200 ms at city t=1961. The open follow-on is the *shadow*-pass bin (row 9). |
| `depth-bounds`, `strip-lists` | 0.18–0.65 / 0.08–0.28 | **No prior history found for either.** Too small to lead a round; note `depth-bounds` reaches 0.646 ms at chase t=800. |
| UNACCOUNTED residue | 1.5–4.1 | §00l.1. Nothing inside it can exceed the residue. |

---

### 00l.6 — EXECUTION ORDER, IF YOU ARE THE NEXT AGENT

Ranked by (measured win) ÷ (risk × effort), not by ms:

1. **Per-scene `frame_tile` default: `setDefault(frame_tile_y, 20)` in
   `CHASE.CPP`.** **MEASURED −3.17 ms of chase's t=800 tick (−5.7 %)**, 327 px of
   2 M move, five hashes to re-pin. §00d already wrote the recommendation and
   nobody executed it. Then offer city 6×20 (−0.79 ms, 4 534 px) to Gil-Ad's eye.
   **Never global — greets loses 0.48 ms and moves 16 % of its pixels.**
2. **Give chase's two `renderFrame` passes distinct `[DPROF]` names, then price
   the `skipVolumetric` ladder.** Upper bound **≈11.7 ms at t=800**. The item is
   named in a 2026-08-17 handover as *"on its face, the largest single perf item
   left in chase"* and has **never been measured**. Step one is instrumentation.
3. **Port `--water_slope_vec8` to `waterWaveSlopeVaried`** — byte-null on the
   city copy, **−34 % of that row's cycles there**, and chase was skipped *by
   construction*. **INFERRED 1.5–2.5 ms** on a 7.97 ms row.
4. **Split `Render_SSAO` into march / blur / apply scopes** (three lines). It is
   the only top-3 row in the map with one profiler line, no `effPar`, and an
   inferred interior.
5. **`--xpar_extent_census` on city t=1961** — one run, decides whether the
   6.51 ms `TBR-render` row is worth opening at all.
6. **`mirror-grid`** — 0.689 ms, serial, flagged three times, never touched.
7. Then the kernel rows (2, 4) — where eleven and seven rounds of refutations
   are waiting for anyone who arrives without reading (b).

### 00l.7 — REPRODUCTION

Drivers are committed at `scratchpad/perfmap28.py` (interleaved, order-rotated,
min-of-rounds, JSON out), `scratchpad/perfmap28_report.py` (the tables above),
`scratchpad/ladder28.py` (the flag ladders) and `scratchpad/binfloor28.py` (the
between-binary floor). All take `PM_ITERS` / `PM_ROUNDS` / `PM_ONLY` / `PM_BIN` /
`PM_LADDERS` / `PM_OUT` from the environment. Single-row recipe:

```sh
cd Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./DEMO \
  --bench=scene@scene=<greets|city|chase>,t=<t>,iters=24 <his arm> \
  --profiler=0 --deferred_prof=1 --strict_flags [--hw_prof]
```

**Do not time SSAO from an `--ssao_dump` run** — the dump forces the scalar apply
loop and inflates the pass ~3.5× (13.7–16.0 ms against ~4.1–4.4 at 1512×848).
The warning is in the source at `DeferredSSAO.cpp:258` and it is repeated here
because two rounds have now had to re-learn it.

---

## 00j. THE PRICE OF `--refl_correct` — 2026-08-17: the mirrored-list build is **1 microsecond a frame**; the cost that exists is the per-vertex normal, and it lives OUTSIDE `renderFrame`

The commissioned look change (`docs/OPTIMIZATION_BACKLOG.md` **2026-08-17**)
has two halves in two places, and they price three orders of magnitude apart.
Parent `DEMO_par` `6acb2ebf…`, child `DEMO_new2` `411af800…`, both built in one
worktree on `cb6aad4c`. **The arm that means anything is `on` vs `off` on the
SAME binary** (flag flipped, LTO layout held fixed); `par` is carried only to
expose the between-binary floor.

### THE MIRRORED-LIST BUILD — 0.001 ms/frame, and that is the whole answer

`--bench=scene@scene=city,t=1961,iters=30 --deferred --profiler=0
--deferred_prof=1`, 5 rounds alternating, min-of-rounds, `wall_avg` ms **per
frame** (the row covers both passes):

| `[DPROF]` row | off | on | delta |
|---|--:|--:|--:|
| **`light-list`** | **0.0100** | **0.0110** | **+0.0010 ms/f (+10 %)** |
| `DeferredLighting-call` | 11.010 | 11.290 | +0.280 ms/f (+2.54 %) |
| `renderFrame` | 55.747 | 56.015 | +0.268 ms/f (+0.48 %) |

**One microsecond a frame**, 0.0018 % of the frame — `ReflMirror_MirrorLights`
is one pass over ~40 lights that appends nothing, so the light count, the tile
binning and every per-pixel loop are untouched. Negligible is the correct word
here and it is measured, not assumed.

Note what the other two rows say: the whole `renderFrame` delta (+0.268) is
inside `DeferredLighting-call` (+0.280), and only 0.001 of that is the list
build. The remaining ~0.279 ms is the per-pixel lighting doing **different
work on different data** — the lights are at mirrored positions, so per-tile
light sets and per-pixel branches differ. That is the feature, not overhead.

### THE COST THAT EXISTS IS THE NORMAL, AND `renderFrame` CANNOT SEE IT

`renderFrame` moves +0.48 % but the whole tick moves ~5× that, because the
`TN`/`TTangent` writes are in `Reflected_Transform` — demo-side, **outside**
`renderFrame`, invisible to every `[DPROF]` row. Two independent whole-tick
instruments, 9 rounds each, interleaved, min-of-rounds:

| instrument | off | on | on vs off | within-arm floor |
|---|--:|--:|--:|--:|
| city `--bench` t=1961, 40 iters (ms/iter) | 71.419 | 73.098 | **+2.35 %** (≈ +1.7 ms/f) | off 1.99 %, on 5.25 % |
| chase two-point wall clock, pose 800, 16→160 frames (ms/frame) | 56.869 | 59.170 | **+4.05 %** (≈ +2.3 ms/f) | off 0.21 %, on −1.32 % |

**Read those floors before quoting the numbers.** The between-binary floor is
larger than the effect in both columns — `par` runs 6.46 % (city) and 6.26 %
(chase) faster than `off` doing *strictly the same work* — and the within-arm
floor reaches 5.25 %. chase's `on` floor came out **negative** (the 2nd-best
sample beat the estimator), which is the two-point estimator telling you it is
not monotone at this precision. So:

* the **direction** is consistent across two instruments and two scenes (`on`
  is slower), and the **mechanism** predicts it — two extra `MatrixXVector`
  per vertex over the reflected pass's ~20–30 k vertices, every frame;
* the **magnitude**, ~2 ms/frame, is real enough to state plainly and is NOT
  dismissed as noise — but it is quoted at one significant figure, because a
  2–4 % effect measured against a 2–7 % floor does not support more;
* the split between the two halves is **not resolvable by this instrument**.
  It does not need to be: the list build is priced directly above at 0.001
  ms/f, so essentially all of the ~2 ms is the per-vertex normal + tangent
  transform. That work IS the feature — it is what a reflected pass needs to
  have a normal at all — and `--no-refl_correct` buys it back exactly.

Machine was under load (1-min average 8.55 at start), which inflates the
floors; the mins are the estimator for that reason.

## 00i. THE SHADOW CLONE'S STALENESS, COUNTED — 2026-08-16t: `Pos` diverges **0 times in 856 M compares**, the item is a CORRECTNESS row not a perf row, and §00h's ceiling re-confirms at **0.56 %**

§00h's hand-on ("`PerTriMeshClone` is never invalidated — worth its own round")
resolved by measurement rather than argument. It is not a perf row: nothing
this round landed moves time, and the perf question it *does* answer is whether
§00h's END-STATE price survives the correctness fix. **It does, to two decimals.**

### The perf gate on what landed (min-of-11, order-rotated, greets t=5743 his arm)

| | DynOmnis wall | floor | DynOmnis core | floor | DynMeshes wall | DynMeshes core |
|---|--:|--:|--:|--:|--:|--:|
| parent | 1.190 | 0.00 % | **10.250** | 0.78 % | 0.210 | 0.680 |
| child | 1.200 | 0.00 % | **10.250** | 0.49 % | 0.210 | 0.670 |

Core-ms — the column with the resolution, being a sum over 42 passes — is
**identical**. The wall column moves one printed LSB and the four columns
disagree in sign, which is what "neutral" looks like here. Mechanism bounds it
independently: the added work is one already-loaded register test per
CLONE-BACKED MESH (~2 226 mesh-visits/frame at this pose) plus two integer
compares on the `cloneOf` map-MISS path — **nothing in any per-vertex or
per-face loop**, which is where §00g showed the time actually is.

### §00h's END-STATE ladder, re-run on the post-fix tree

| arm | DynOmnis wall | floor | DynOmnis core | floor | DynMeshes wall |
|---|--:|--:|--:|--:|--:|
| **32** — ships | 0.870 | 0.00 % | 7.340 | 0.68 % | 0.170 |
| **288** — replica CONTROL | 0.860 | 0.00 % | 7.180 | 2.09 % | 0.170 |
| **1568** — END STATE | **0.610** | 1.64 % | **4.980** | 0.00 % | **0.140** |

**−29.1 % wall / −30.6 % core vs the control**; Δ 0.250 + 0.030 = **0.280
ms/frame = 0.56 % of a 49.6 ms greets frame** — §00h measured 0.56–0.63 %.
The correctness work does not move the price, and it strengthens the read half's
byte case (see below), so if the row is ever re-opened it re-opens at 0.56 %.

### The correctness numbers, for the record

`--clone_stale_census`, greets, his arm, 13-pose sweep with `FDS_GREETS_SHATTER=1`
(so the shatter's 238-shard / 12-worker reflection bake — the second clone-backed
pass — is live): **630 622 clone reuses, 856 176 679 vertex compares,
285 626 101 face compares. `Pos` = 0, `N` = 0, `Tangent` = 0, size-drift = 0.**
Only `BGRA` (355 630 633) and `Face::EU1..EV3` (644 742, all `__discoBall`)
diverge, and both are rewritten downstream. Refreshing every one of them
(`--clone_refresh_inputs`) is byte-identical across 8 greets configurations.
**city / chase / fountain run ZERO clone-backed passes** — the inertness control
for this whole subject, and the same greets-only blast radius §00g measured.

Full account: `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16t**.

---

## 00h. SoA PHASE 5, PRICED BY BUILDING THE LOOP INSTEAD OF THE STRUCT — 2026-08-16s: **0.6 % of a greets frame, not 1.25 %**, and the variable is the 209.6 MiB shadow CLONE, not `sizeof(Vertex)`

**§00g handed Phase 5 on at 1.25 % of frame by extrapolating a byte-slope
(0.0086 ms/B) from a struct that had been *inflated* 140 → 192 down to the
140 → 68 end state. That extrapolation is refuted. Measured end state: 0.56-0.63 %.**
The extrapolation was never testable in-tree — nothing can shrink `Vertex`
without the refactor — so the **per-vertex loop was rebuilt as a ladder** and the
end state timed directly. Status: **Phase 5 BLOCKED ON SCOPE** (274 of the
migration's references live in DEMO scene code, including three alternative
transform pipelines); one instrument landed; a better-shaped successor item
specified. Full write-up: `docs/SOA_VERTEX_REFACTOR.md`, top section.

### THE INSTRUMENT — `--xfrm_ablate` bits 256…16384, census build only

Replicas of the per-vertex loop that differ **only in where the read and the
write land**. Verified in the disassembly of `_Transform_Objects`: identical FP
sequence (3× `fmla.4s`, `fdiv`, 2× `fmul`) in every arm, and the read-source
select is **unswitched out of the loop** (`tbz w10,#0xa` above the back-edge)
with both strides advanced per iteration — `#0x8c` (the 140-byte `Vertex`) and
`#0x20` (the dense 32-byte record). So the arms are an addressing change, which
is what makes the ladder a memory measurement.

### THE LADDER — greets t=5743, his acceptance arm, 1920×1080, DynOmnis phase-A `xform`, face loop ablated (`|32`)

Per-frame min over 24 frames, min over 5 order-rotated rounds, dummy drivers.

The three arms that carry the verdict are **min-of-11 with their noise floors**
(floor = (2nd-min − min)/min); the two half-arms are min-of-5.

| arm | `Pos` read from | outputs written to | DynOmnis wall | floor | core-ms | floor |
|---|---|---|--:|--:|--:|--:|
| **32** — what ships | per-light clone `Vertex` (140 B) | the same record | **0.870** | 0.00 % | **7.130** | 0.70 % |
| **288** — replica CONTROL | same | same | **0.840** | 1.19 % | **7.200** | 0.42 % |
| 544 | clone `Vertex` | dense **32 B/vert** | 0.99 **(+16 %)** | 1.01 % | 8.86 | 0.45 % |
| 1056 | **shared `T->Verts`** | clone `Vertex` | 1.13 **(+33 %)** | 1.77 % | 9.86 | 0.20 % |
| 2080 | compact **shared 12 B/vert** `Pos` | clone `Vertex` | 0.87 **(0 %)** | 0.00 % | 7.51 | 2.04 % |
| **1568 — the END STATE** | **shared `T->Verts`** | **dense 32 B/vert** | **0.600 (−28.6 %)** | 1.67 % | **4.870 (−32.4 %)** | 2.26 % |

`DynMeshes`, same runs: **0.170 → 0.130 wall (−23.5 %)**, floor 0.00 % both.
**Signal-to-floor on the verdict row is 17×.**

**The control row is what licenses the rest** (0.840 vs 0.870 wall, 7.200 vs
7.130 core — inside 3.5 %, and in opposite directions on the two columns): the
replica is the shipping loop, and any branch it carries is carried by every arm
and cancels. **Then read 544 / 1056 / 2080 — each is one HALF of
Phase 5, and every half alone is neutral or worse.** A `sizeof(Vertex)` model
predicts monotone improvement as the walked record shrinks; arm 544 takes the
28 written bytes *out* of the record and costs **+16 %**.

### WHY — `--mem_census` names it in one line

```
[MEM] 219 818 480  209.64 MiB  shadow.scratch/per-light mesh clones (Vertex[])
                                1269 live (shadow map x mesh) clones x VIndex x sizeof(Vertex)=140
```

42 concurrent bakes cannot write one shared `Vertex`, so each (light-face ×
mesh) pair gets a **full copy** — and **68 of every 140 bytes of those 209.6 MiB
are read-only duplicates** (`Pos`/`N`/`Tangent`/UV/bary). Today's loop is ONE
stream over that. Splitting only the write makes two streams over the same cold
209.6 MiB (+16 %); moving only the read gives two 140-byte strides (+33 %);
doing both collapses the read onto the single `T->Verts` (~15 MiB, warm across
all 42 bakes) and makes the write dense — **that pair, and only that pair, is
the −31 %.** The lever is the clone.

### THE MAIN VIEW — ±5 %, neutral. No clone exists there, so there is nothing to collapse.

`--xfrm_par=0 --xfrm_prof`, VERT bucket, greets t=5743, min over 5 rotated rounds:
ships **0.963**; replica control (4096) **0.911**; dense write only (8192)
**0.959 (+5.3 %)**; dense write + compact read (16384) **0.882 (−3.2 %)**.
−0.029 ms serial, and the shipping path is parallel (0.48 ms wall for the whole
call), so the realised delta is smaller. **Inside noise in both directions** —
Phase 5 neither pays nor regresses here.

### PREDICTION vs MEASUREMENT, and it reproduces at every bake-heavy pose

| | ms/frame | % of a 49.59 ms frame |
|---|--:|--:|
| **PREDICTED** (§00g: 72 B × 0.0086 ms/B) | 0.62 | **1.25 %** |
| **MEASURED**, min-of-11 vs the replica control | **0.28** | **0.56 %** |
| same, vs the shipping arm | 0.31 | 0.63 % |

**Over-predicted 2.0–2.2×**, outside the ±20 % bar, with a named cause: the slope
was calibrated by *stretching a one-stream walk* and extrapolated to a
*two-stream* end state — a different access pattern, not a shorter one.

Cross-pose consistency (min-of-3/5 per pose, arm 32 → arm 1568):

| pose | DynOmnis | DynMeshes | Δ ms/f | frame min | **% of frame** |
|---|--:|--:|--:|--:|--:|
| t=5743 | 0.85 → 0.59 | 0.17 → 0.13 | −0.30 | 49.59 | **0.61 %** |
| t=2845 | 0.89 → 0.61 | 0.17 → 0.14 | −0.31 | 50.51 | **0.61 %** |
| t=1588 | 1.05 → 0.70 | 0.19 → 0.14 | −0.40 | 60.78 | **0.66 %** |
| t=6097 (no RTT) | 0.87 → 0.63 | 0.17 → 0.13 | −0.28 | 41.26 | **0.68 %** |

### THE CHEAP SHORTCUT IS BYTE-NULL **AND** WORTH ZERO

Swapping the shadow loop's `Pos` read from the clone to the shared `T->Verts` is
**byte-null** — greets t=5743 `818f0336…`, t=1588 `756790e4…`, arm 256 vs arm
1024, control repeated, identical — and worth **0 %** (arm 2080). The read was
already free; the line comes in for the *write* regardless. **No bit-exact
subset of Phase 5 pays.**

Latent hazard found on the way, unrelated to perf: **`PerTriMeshClone` is never
invalidated** (grepped — nothing clears `VertexScratch::clones` or resets
`initialized`), so a clone's `verts` is a first-bake snapshot including `Pos`,
while five files write `Vertex::Pos`. Not stale at the poses measured; nothing
enforces it.

### GATES (the instrument is census-only)

* **The shipping binary is byte-identical to the parent's** — `md5 f5cc3479…`
  before and after, which is a stronger statement than any pin.
* 11 pin recipes **3/3, parent-identical**; ten at their recorded 16f/16r values
  (city `4cb8d2ca` / `f473fe2b` / `d3374de6`, chase `3bfd4244` / `42d79fad` /
  `622b96a2` / `31aa5203` / `ca07a814`, fountain `8db68ccb`, greets t=1588
  `570a7b44`) plus the four greets acceptance poses (`26ad272a` / `10adec3a` /
  `418fc1fa` / `6d02f31b`).
* `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
* `--shadow_plane_hash` stable 2/2 (`03587397…` over all 43 bakes).
* Renders eyeballed, one per scene:
  `docs/img/soa5/{greets_t_5743,city_t_1961,chase_t_800,fountain_t_2500}.png`.

---

## 00g. `Transform_Objects` DECOMPOSED — 2026-08-16r: the row is not shared machinery, it is greets' shadow bake (81 % of it), and what is left is memory-bound to the byte

> **AMENDED 2026-08-16s (§00h above): this section's hand-on — "Phase 5's
> ceiling is 1.25 %" — is an extrapolation of the byte-slope below in the
> direction it was never measured. Built and timed, the end state is 0.56-0.63 %,
> and the variable is the 209.6 MiB per-light clone, not `sizeof(Vertex)`.
> Everything else here (42 of 45 calls, the ablation ladder, the six
> refutations) stands and is what located the clone.**

**16q handed on "`Transform_Objects` is 3.35 % of DEMO self samples at greets
t=5743, more than double what is left of the clipper". It is — and at city it is
0.309 % and at chase 0.190 %.** The symbol runs **45 times a frame** at greets and
**42 of those calls are the shadow bake**, which no other scene in the battery
runs at all. Decomposed to the instruction, then attacked: **six mechanisms
refuted by measurement (four of them the obvious ones), one landed**
(`--greets_displace_offscreen_skip`, default ON, byte-exact at 11 pin recipes),
and the row's remaining 1.9 ms/frame priced as a single function of
`sizeof(Vertex)` with a measured slope.

### THE SYMBOL, BY SCENE — it is not shared machinery

`sample <pid>`, self samples as a share of DEMO's (`scratchpad/selftime2.sh` +
`sumself.py`, §00e's instrument), one pose per process:

| arm | `Transform_Objects` | `FrustumClipper::Render` |
|---|--:|--:|
| **greets t=5743, his acceptance arm** | **3.363 %** (16q: 3.349 — reproduced) | 1.417 % |
| city t=1961, his acceptance arm | **0.309 %** | 1.349 % |
| chase t=800 `--deferred` | **0.190 %** | 1.054 % |

**The 3.35 % row is a greets row.** city, chase, fountain and crash bake no
shadow map (16q measured that as a property of four scenes), and without the bake
this symbol is a fifth of the clipper rather than double it.

### THE SPLIT BY INVOCATION SOURCE — `--xfrm_pass_prof`, and it needed new columns

The per-pass census already existed (`docs/VISIBILITY_PLAN.md` §8a) but had never
been pointed at this question, and it does not dump under the shipping
`--xfrm_par` default — the main-view call early-returns into the sharded driver
*before* the accounting, and the shards set `_xpassN = 0`. Run with
`--xfrm_par=0`; census build (`-DFDS_VIS_CENSUS=ON`), greets t=5743, his arm,
per frame:

| pass | calls/f | core-ms | meshes xf/seen | verts xf/seen | fTested | visRej | fPushed |
|---|--:|--:|--:|--:|--:|--:|--:|
| MAIN | 1.00 | 1.93 *(serial)* | 105 / 221 | 201 751 / 325 474 | 67 560 | 14 439 (21.4 %) | 36 413 |
| MIRROR-RTT | 2.00 | 0.83 | 68 / 442 | 105 914 / 597 552 | 35 648 | 33 158 (93.0 %) | 113 |
| **SHADOW** | **42.00** | **11.78** | 425 / 2 982 | **596 446** / 4 312 518 | **203 622** | 110 385 (54.2 %) | 41 871 |
| OFFSCREEN (env/SH probes, intermittent) | 0–2.7 | 0–2.29 | 179 / 600 | 257 604 / 894 992 | 86 503 | 31 616 (36.5 %) | 3 169 |
| **all** | **45** | **14.55** | | **904 111** | **306 830** | | 78 397 |

**42 of 45 calls, 596 446 of 904 111 transformed vertices and 81 % of the core
time are the shadow bake.** The 42 are 16q's map inventory: 18 cube faces of 3
moving omnis @128² + 10 spot maps @256² (`DynOmnis`, full geometry), plus 14
static cube faces @512² (`DynMeshes`, dynamic meshes only). The mesh culls throw
away **86.2 %** of the vertices the shadow pass sees before transforming any of
them, which is why 4.31 M seen becomes 596 k done.

WALL time per source, shipping build (`--shadow-prof`, `--xfrm_prof`):

| source | wall ms/frame | note |
|---|--:|---|
| SHADOW phase A, `DynOmnis` | **1.22** | 28 maps |
| SHADOW phase A, `DynMeshes` | **0.22** | 14 maps; mostly its own 10.5 MB plane clear |
| MAIN (`--xfrm_par`, 26 shards) | **0.48** | PLAN 0.008 / WORK 0.424 / COMPACT 0.023 / EPI 0.006 |
| **row total** | **≈1.9** | of a 49.5 ms frame = **3.8 %** |

### THE NEW NUMBER THIS PHASE WAS NEVER MEASURED BY — `xformCore` / `effPar`

`--shadow-prof` now sums the per-light `Transform_Objects` durations across
whatever worker ran them and prints them against the phase's wall time (two clock
reads per light per frame, only under the flag):

| bake | xformCore ms/f | wall ms/f | **effPar** |
|---|--:|--:|--:|
| `DynOmnis` | 10.4–10.7 | 1.21–1.22 | **8.4–8.7** |
| `DynMeshes` | 0.67–0.75 | 0.22 | 3.1–3.4 |

**This box is 8 P-cores + 4 E-cores** (`hw.perflevel0.logicalcpu` = 8,
`hw.ncpu` = 12). 8.6 is the P-core count plus a little E-core help, and the wave
bound for 28 jobs over a 12-worker pool is 9.33 anyway. **Scheduling is refuted by
arithmetic** — this is not the LGHT round's idle-pool disease.

### THE INTERIOR — the ablation ladder, run in the SHADOW pass for the first time

`--xfrm_ablate` was main-view-only by construction (`_mainView`, `xresOverride <
0`), so it could not see the pass that dominates the symbol. The census build now
runs it in **every** pass (`#if FDS_VIS_CENSUS`, textually absent from the
shipping build) and a new bit 128 skips the vertex loops, so bits 32 and 128
bracket the two halves from opposite sides. `DynOmnis` xform wall ms/frame:

| arm | ms | derived |
|---|--:|---|
| baseline | 1.22 | |
| 160 = neither half | 0.07 | **residue R = 0.07** (dispatch + 2.7 MB plane clear + the 19 782-step object walk + per-mesh setup + culls) |
| 32 = no face loop | 0.90 | **vertex loops V = 0.83** |
| 16 = face loop is a pure pointer walk | 0.91 | Face walk = 0.01 |
| 8 = walk + visibility/backface test | 1.17 | **the test = 0.26** |
| (baseline − 8) | | accepted-face work (SortZ + FList push + bbox stamp + reflective/xpar) = 0.05 |
| 64 = no tile-bbox stamp | 1.24 | the bbox stamp = **0.00** |
| 2 = no 1/z + PX/PY | 1.21 | the projection block = 0.01–0.03 |
| 4 = no SoA stores at all | 1.14 | all four SoA stores = 0.10 |

0.83 + 0.01 + 0.26 + 0.05 + 0.07 = 1.22. **The four buckets sum.**

The PC-offset histogram agrees and localises the middle one to a single
instruction: **+9864 is 31.6 % of the symbol's self samples** — the loop advance
immediately after `tst w11,#0x3f ; b.eq`, i.e. `Face::VisibilityFlagsAll()`'s
three random `Vertex::Flags` derefs and the branch that decides on them. There is
exactly **one** face loop in the binary (one `add x28,x28,#0xaa`, `sizeof(Face)`
= 170) against 14 vertex-advance sites, so that 31.6 % spans every pass; at city,
which bakes no shadow, the same offset is **6.5 %**.

### SIX REFUTATIONS, EACH WITH THE MEASUREMENT

1. **SIMD width / arithmetic.** The whole projection block (`1/z` + PX/PY) is
   0.01–0.03 ms of 1.22. `--shadow_cube_vert_cull` — which replaces the 3-FMA
   matmul with a world-space pyramid test for out-of-face vertices — moves the
   core time **10.67 → 10.77 ms**, i.e. nothing. The loop is not ALU-bound and
   widening it cannot pay.
2. **Scheduling.** effPar 8.4–8.7 against 8 P-cores; see above.
3. **`--shadow_cone_cull`** (default OFF, the mesh-bsphere-vs-spot-cone cull, and
   10 of the 28 `DynOmnis` maps are spots): ON gives **10.67 → 10.55** core-ms.
   Inside the floor. It stays off.
4. **`--shadow_cube_face_cull`** (default ON): OFF gives **10.67 → 13.15**
   (+23 %). The cull is live and worth 2.5 core-ms/frame — there is nothing dead
   to reclaim there, and this is the control that says the culls are already
   doing their job.
5. **More main-view shards.** `--xfrm_par` 26 / 52 / 104 → WORK **0.445 / 0.450 /
   0.443 ms**. Flat. The main view is not LPT-bound at this pose any more; it is
   at the same wall the shadow pass is.
6. **Dropping the three DEAD SoA arrays in the shadow pass.** Only `TPos_z` is
   read there (`Shadows.cpp`'s all-behind reject, `F->frame->TPos_z[A_idx…]`);
   `TPos_x`, `TPos_y` and `PY` have no shadow-pass reader. The arm that removes
   **all four** buys 0.10 ms, so three of four is ≈0.075 ms = **0.15 % of frame**,
   and it would cost a branch inside the shared `-ffp-contract=fast` vertex loop.
   Refused by arithmetic.

### THE PREDICTION THAT FAILED, AND IT IS THE FINDING

The shadow pass touches exactly two windows of the 140-byte `Vertex`: `[0,28)`
written (PX, PY, Flags, TPos_AOS, RZ) and `[52,64)` read (Pos) — the TN/TTangent
block at `[28,52)` is `!_inShadowPass` and skipped. That is a **64-byte hot window
at a 140-byte stride**, which straddles two cache lines ~98 % of the time.
**Predicted: pad `sizeof(Vertex)` to 192 (a multiple of 64), the window becomes
exactly line 0, the lines touched per vertex halve.** Measured with the
`-DFDS_VERTEX_PAD_BYTES` hook the SoA doc already shipped:

| `sizeof(Vertex)` | 64-aligned? | `DynOmnis` xform ms | xformCore ms |
|---|---|--:|--:|
| **140 (shipping)** | no | **1.21** | **10.37** |
| 144 | no | 1.23 | 10.83 |
| 160 | no | 1.33 | 11.59 |
| **192** | **yes** | **1.58** | **12.37** |

**Monotone in SIZE; the aligned arm is the worst of the four.** The line-straddle
theory is dead and `docs/SOA_VERTEX_REFACTOR.md` §3's controlled experiment
("`sizeof(Vertex)` is the ONLY variable this loop responds to") reproduces in the
shadow pass, where it had never been run. Slope: **0.0071 ms per byte** on
`DynOmnis`, **0.0086 ms/byte** counting `DynMeshes`.

### THE ROW THIS HANDS ON — Phase 5's ceiling is 4× what the doc closed it on

`docs/SOA_VERTEX_REFACTOR.md` closed Phase 5 (`sizeof(Vertex)` 140 → 68) on
2026-08-09 at **"0.24–0.31 % of frame"** — measured, in that doc's own words, on
`--xfrm_prof` buckets, which are **main-view only**. The shadow pass is 3× the
main view's vertex count and 81 % of this symbol. At the slope above, 72 bytes is
**0.62 ms/frame = 1.25 % of a 49.5 ms greets frame** — four to five times the
number that closed it. The verdict (11 files, two alternative transform
pipelines, every writer must be found) may well stand; the *number* in that doc
does not, and whoever re-opens it should quote this one.

### WHAT LANDED — `--greets_displace_offscreen_skip`, default ON

The one piece of dead work the census did find, and it is a flag state, not a
loop. `--greets_displace` splits the greets stone into chunks and tags the
displaced detail `Face_MainOnly`; a chunk that is **100 % displaced** casts no
shadow of its own (the flat `Tri_OffscreenProxy` stand-in does) and already got
`Tri_NoShadowCast` at scene init — 149 pure chunks, 65 179 faces. But
`Tri_NoShadowCast` is gated on `g_inShadowPass`, so it spared the shadow bake
**and nothing else**: the mirror RTT bakes and the env/SH probes are offscreen
passes too, and they were transforming those chunks in full and then dropping
every one of their faces on `Face_MainOnly`. Measured, greets t=5743, his arm:

| pass | verts on all-`Face_MainOnly` meshes | of its transformed verts | face-visits dropped on `Face_MainOnly` |
|---|--:|--:|--:|
| MIRROR-RTT | **54 073/f** | 105 914 (**51.1 %**) | 31 894 of 33 866 (**94.2 %**) |
| OFFSCREEN (env/SH probes) | **151 500/f** | 257 604 (**58.8 %**) | 73 946 of 86 503 (85.5 %) |
| SHADOW | 0 | — | 105 675 of 197 348 (mixed chunks; those still cast) |
| MAIN | 0 | — | 0 |

`Tri_AllFacesMainOnly` is the offscreen-wide form of the same fact, stamped by
the chunk split next to `Tri_NoShadowCast` and tested in the mesh loop against
the OFFSCREEN predicate. **Byte-exact by construction**: every face of such a
mesh carries `Face_MainOnly`, so in an offscreen pass the face loop `continue`s
on all of them and the mesh emits not one FList entry — the same "no faces => no
output" invariant the existing `FIndex == 0` skip rests on. The hatch is at
**scene init**, not in the mesh loop: a runtime flag read inside that
`-ffp-contract=fast` function is not byte-null even when never taken
(`docs/VISIBILITY_PLAN.md` §8).

### THE PRICE — min-of-11, order ROTATED, one pose per process, three arms

`scratchpad/xform_ladder.sh` (new). The change lives in the OFFSCREEN passes,
which run in greets' **ANIM** profiler block (`UpdateAllMirrors` +
`RenderSecondOrderMirrors` + the probe bakes, `DEMO/GREETS.CPP:3686`) and
therefore **outside `renderFrame`** — so `renderFrame`'s `Ginstr/f` is this
round's inertness control and ANIM is the column that carries the result. Round 0
dropped, 11 rounds, three arms (parent `8dde99fd` / child `--no-...` / child
default), 1512×848, `--profiler=1 --deferred_prof=4 --hw_prof`. Noise floor per
column = max over arms of (2nd-min − min)/min.

| pose | column | parent | child OFF | **child ON** | floor |
|---|---|--:|--:|--:|--:|
| **greets t=5743** (his arm) | **ANIM ms** | 2.234 | 2.249 | **1.870 (−16.3 %)** | 2.19 % |
| | TOTL ms | 64.578 | 64.697 | **63.500 (−1.67 %)** | 0.60 % |
| | frame min ms | 49.590 | 49.420 | **49.290 (−0.60 %)** | 0.51 % |
| | `renderFrame` Ginstr/f | 4.683 | 4.681 | 4.680 | 0.04 % |
| | `gbuffer` Ginstr/f | 0.992 | 0.992 | 0.992 | 0.10 % |
| **greets t=2845** | **ANIM ms** | 2.718 | 2.724 | **2.381 (−12.4 %)** | 0.46 % |
| | TOTL / frame min | 65.695 / 50.510 | 65.825 / 50.520 | **64.378 (−2.00 %) / 50.060 (−0.89 %)** | 0.44 / 0.44 % |
| | `renderFrame` Ginstr/f | 4.718 | 4.717 | 4.716 | 0.04 % |
| **greets t=1588** (his arm) | **ANIM ms** | 2.924 | 2.953 | **2.498 (−14.6 %)** | 1.13 % |
| | TOTL / frame min | 76.831 / 60.780 | 76.696 / 60.810 | **74.801 (−2.64 %) / 59.650 (−1.86 %)** | 0.64 / 0.07 % |
| | `renderFrame` Ginstr/f | 6.202 | 6.203 | 6.201 | 0.02 % |
| **greets t=6097** | ANIM ms | **0.041** | 0.040 | 0.040 | 7.5 % |
| | frame min | 41.260 | 41.390 | 41.550 | 0.29 % |
| **greets t=1588, bare `--deferred`** | ANIM ms | 2.169 | — | 2.127 (−1.9 %) | 3.71 % |
| | `renderFrame` Ginstr/f | 4.177 | — | 4.177 | 0.00 % |
| **city t=1961** (his arm) | frame min / Ginstr | 45.750 / 4.175 | — | 45.900 / 4.177 | 0.42 / 0.02 % |
| **fountain t=2500** | frame min / Ginstr | 35.620 / 1.062 | — | 36.380 / 1.061 | 3.46 / 0.00 % |

**Read the ANIM column, and read the two null poses as the controls they are.**
At t=6097 the mirror RTT does not run at all (ANIM is 0.041 ms — 60× smaller than
the other poses) and the change is inert; at t=1588 without `--greets-displace`
nothing sets `Face_MainOnly`, so no chunk is ever stamped and ANIM moves −1.9 %
against a 3.71 % floor. Those two, plus city and fountain, bracket the
cross-binary noise at roughly ±0.7 % on `frame min` and ±2 % on TOTL — which is
why the frame-level rows above are quoted but **not leaned on**. The column with
signal is ANIM: **−12 to −16 % at all three poses where the RTT runs, 0 % at both
poses where it does not**, and `renderFrame` never moves by more than one printed
LSB of instructions at any of them.

**The child's OFF arm is null everywhere** — ANIM +0.67 / +0.22 / +0.99 %, all
inside the floor, `renderFrame` Ginstr/f within 1 LSB — which is what says the
`Tri_AllFacesMainOnly` mesh-loop line and the `xformCore` clock reads cost
nothing when the bit is never stamped.

### GATES

* **11 pin recipes, 3/3 each, parent `8dde99fd` vs child, one worktree, one asset
  tree**: city `bd4ffbf8` / `4cb8d2ca` / `f473fe2b` / `d3374de6`, chase
  `3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain
  `8db68ccb` — **all ten at their recorded 16f values** — plus greets t=1588
  `570a7b44` and the four greets acceptance poses (t=5743 `26ad272a`, t=2845
  `10adec3a`, t=6097 `418fc1fa`, t=6133 `6d02f31b`), differential and identical.
  **The acceptance poses are the ones with teeth**: they carry
  `--greets-displace`, the only arm in which this change does anything at all.
* **`--shadow_plane_hash` identical** parent vs child, greets t=5743, his arm, 43
  bakes, running digest equal at every `seq` — 16q's instrument, used because
  this round touches `Shadows.cpp`.
* `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` /
  `166fa25a`). What it exercises here, measured with `--xfrm_pass_prof` rather
  than assumed: `conetest` runs **MAIN 1 call (1 624 verts, 272 faces pushed) +
  SHADOW 1 call (1 624 verts, 44 pushed)**, `mirrortest` MAIN 1 call (25 meshes,
  752 verts, 334 pushed), `halotest` MAIN 1 call (4 verts). **No gate scene sets
  `Face_MainOnly`** — only `--greets_displace` does — so the gate is an
  inertness control for this change, not a discriminator; the discriminating gate
  is the four greets acceptance pins.
* One render eyeballed per scene: `docs/img/xform/greets_t005743_color_xform.png`,
  `city_t001961_color_xform.png`, `chase_t000800_color_xform.png`,
  `fountain_t002500_color_xform.png` — all correct.

### METHOD NOTE, because it cost a run

**chase's recorded pins reproduce WITHOUT `--profiler=0`; adding it gives a
different, self-consistent set of five.** 16f's "the flag is inert on snapshots
again" holds for city / fountain / greets and **not** for chase. Run chase's
recipe verbatim.

### THE RANKED REMAINDER OF THIS SYMBOL, greets t=5743

| # | item | wall ms/f | % of frame | verdict |
|---|---|--:|--:|---|
| 1 | shadow-pass per-vertex loops | 0.91 | 1.8 % | memory-bound to the byte; only lever is `sizeof(Vertex)` (Phase 5, ceiling 0.62 ms) |
| 2 | main-view `Transform_Objects` | 0.48 | 1.0 % | same wall; shard count is flat 26→104 |
| 3 | per-face `VisibilityFlagsAll` in the shadow pass | 0.26 | 0.53 % | 3 random `Vertex::Flags` derefs, 54.2 % of them buying a reject. A compact per-mesh `uint8_t` flags array would move the rejected half only (the accepted half needs the same line for the bbox stamp) ⇒ ≈0.16 ms = 0.32 %. **Below bar, parked** |
| 4 | shadow phase-A residue | 0.07 | 0.14 % | of which the 13.2 MB/frame plane clear is 0.51 core-ms; `sm.dirtyX0..Y1` already records what the last bake wrote, so a dirty-rect clear is byte-exact and available. Below bar |
| 5 | accepted-face work (SortZ, FList push, bbox stamp) | 0.05 | 0.10 % | the S2 bbox stamp itself measures 0.00 |
| 6 | the projection block (`1/z`, PX/PY) | 0.01–0.03 | — | refuted |

**None of items 3–6 clears 0.5 % of frame at any pose, and items 1–2 are one
mechanism with one lever.** This is §00e's shape again: the symbol is not one
expensive thing, it is a memory wall plus five cheap ones.

---

## 00f. THE SHADOW RASTER GETS THE PRE-REJECT IT NEVER HAD — 2026-08-16q: 231 735 clipper entries/frame → 41 787, and the reject is per-TILE, not per-map

**§00e handed on one row: `Shadows.cpp`'s depth raster is 83.4 % of greets'
clipper entries and 81.8 % of ITS entries (189 567/frame) are clipped away to
nothing, because that walk has no screen-bbox pre-reject at all. It has one now
— `--shadow_bbox_cull`, DEFAULT ON.** At greets t=5743 on his acceptance arm the
shadow raster's clipper population goes **231 735 → 41 787 entries a frame
(−82.0 %)** and its clipped-away-to-nothing count goes **189 567 → 22**; the
whole frame's clipper population goes 277 777 → 87 829. `FrustumClipper::Render`
self time goes **2.265 % → 1.429 % of DEMO self samples** (§00e's 2.270 %
reproduced to three decimals first). The `DynMeshes` bake's RASTER half goes
**1.31 → 0.78 ms/frame (−40 %)**, and the frame minimum **−0.8 to −2.3 %**
depending on pose.

Byte-exact, and proved three ways rather than argued: **43–59 packed shadow
planes per pose byte-identical** under a new `--shadow_plane_hash` across eight
greets poses + conetest + the shatter; a **direct counter-example probe**
(`-DFDS_SHADOW_BBOX_VERIFY=ON`) that computes the reject, does NOT apply it, and
counts polygons the raster receives from a face the reject would have thrown
away — **79.7 million rejectable face-visits, 0 polygons**; and the standard
battery (11 pin recipes 3/3 parent-identical, `render_gate.sh` 4/4).

### THE CENSUS, REPRODUCED FIRST — §00e's numbers land to the digit

`--clip_stats`, `(iters=28 − iters=8)/20` on the `--bench` arm, greets t=5743,
`--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao
--greets-displace`. §00e's cumulative TOTAL of 8 499 390 at iters=28 reproduces
EXACTLY on this box, and so does every shadow figure.

| `Shadows.cpp` depth raster, per frame | parent `5071cc37` | child, `--no-shadow_bbox_cull` | **child, default** |
|---|--:|--:|--:|
| entries | 231 735 | 231 735 | **41 787 (−82.0 %)** |
| no-clip (wholly inside the tile) | 31 120 | 31 120 | **31 120 — IDENTICAL** |
| emitted (the clip made a vertex) | 11 048 | 11 048 | 10 645 |
| **rejected (bought no pixels)** | **189 567 (81.8 %)** | 189 567 | **22 (0.05 %)** |
| whole-frame clipper entries | 277 777 | 277 777 | **87 829** |
| `needZ`, whole frame | 14 514 | 14 514 | **14 514 — IDENTICAL** |

**The two IDENTICAL rows are the result, not decoration.** `no-clip` is the
population that produces pixels with no clipping at all — not one of them is
lost. `needZ` is the near/far clip, where §00e measured 51 % of greets' clipper
self time — also not one lost, because a vertex behind the light camera's near
plane leaves the cover-all sentinel box and can never be rejected. **The cull
removes exactly the 2-D-reject population and nothing else**, which is why the
time saved (−37 % of the symbol) is smaller than the entries removed (−82 %).

`emitted` drops by 403/frame and that is not a loss either: `emitted` counts
"the clip manufactured a vertex", NOT "the face drew". A face straddling the
tile in X and wholly outside it in Y manufactures vertices at `Left`/`Right`
and is then annihilated by `Up`/`Down` — it was always in the bought-no-pixels
set, just filed under a different column.

The pre-reject's own census (new `[CLIP] shadow walk` line): the walk sees
**237 609 (face, tile) pairs a frame**, rejects **190 365 (80.1 %)**, and
**9 338 (3.9 %)** carry the cover-all sentinel and are structurally unrejectable.

### WHERE THE REJECT LANDED, AND WHY IT IS NOT WHERE §00e GUESSED

§00e's caveat offered three levels — per-tile bbox, bbox-vs-map-rect, or a
light-frustum reject — and warned the shadow "tile" is often the whole map.
**Measured: it is the per-TILE reject, and the other two are worth exactly
zero.** `FDS_SHADOW_TILE_GRID=1` forces one tile per map, i.e. makes the tile
rect BE the map rect, and the counter-example probe then reports **0 rejectable
face-visits in 42 bakes**. No face in a per-light FList has a box that misses
its map: the light-frustum-level cull is ALREADY done upstream, by
`Transform_Objects`' spot-cone / cube-face bsphere culls and the walk's own
all-behind reject. Everything this row buys is *inside* a map the face really
does overlap.

Which makes the win a pure function of the tile grid, and `gridFor(res)` makes
the grid a function of the map resolution (`res>>7`, clamped [1,4]):

| greets bake | maps | res | tiles/map | rejectable visits (42 bakes) | raster Δ |
|---|--:|--:|--:|--:|--:|
| `DynOmnis` | 18 @128² + 10 @256² | 128/256 | 1 / 4 | small | **−4.0 %** (floor 1.65 %) |
| `DynMeshes` | 14 | 512² | **16** | 4.2 M | **−40.0 %** |
| `StaticOnce` (init) | 48 | 512² | **16** | — | entries ≈ 507 k → 104 k |
| `FDS_SHADOW_TILE_GRID=8` (probe) | — | — | 64 | **43.1 M** | — |

The 128² maps are single-tile and get nothing — which is exactly why the
`DynOmnis` column does not move and the `DynMeshes` column moves by 40 %.

### THE PRICE — min-of-11, order ROTATED, one pose per process

`scratchpad/shadowbbox_ladder.sh` (new; `scratchpad/ladder.sh` is BLIND to this
change by construction — the shadow bake is not inside `renderFrame`,
`shadow-join` is 0.002 ms). Round 0 dropped, 12 rounds, 3 arms, 1512×848,
`--profiler=1 --deferred_prof=4 --hw_prof --shadow-bake-time --shadow-prof`,
`FDS_SHADOW_PROF_INTERVAL=5` and the MIN over the printed intervals. Noise floor
per column = max over arms of (2nd-min − min)/min.

| pose | column | parent | child OFF | **child ON** | floor |
|---|---|--:|--:|--:|--:|
| **greets t=5743** (his arm) | frame min ms | 49.92 | 50.10 | **49.51 (−0.82 % vs parent)** | 0.57 % |
| | TOTL ms | 65.01 | 65.36 | 64.72 | 0.38 % |
| | `DynMeshes` RASTER ms | 1.31 | 1.30 | **0.78 (−40.0 %)** | 3.85 % |
| | `DynMeshes` bake ms | 1.55 | 1.55 | 1.03 (−33.6 %) | 2.91 % |
| | `DynMeshes` XFORM ms | 0.20 | 0.21 | 0.21 | 5.00 % |
| | `DynOmnis` RASTER ms | 1.24 | 1.26 | 1.21 (−4.0 %) | 1.65 % |
| | `renderFrame` Ginstr/f | 4.682 | 4.681 | 4.682 | 0.02 % |
| | `gbuffer` Ginstr/f | 0.992 | 0.992 | 0.992 | 0.00 % |
| **greets t=1588** (`--deferred`) | frame min ms | 41.65 | 41.62 | **40.69 (−2.30 %)** | 0.10 % |
| | `DynMeshes` RASTER ms | 2.06 | 2.08 | **1.21 (−41.3 %)** | 1.94 % |
| **greets t=6097** | frame min ms | 41.76 | 41.55 | **41.30 (−1.10 %)** | 0.84 % |
| | `DynMeshes` RASTER ms | 1.17 | 1.21 | 0.75 (−35.9 %) | 4.00 % |
| **greets t=6133** | frame min ms | 41.85 | 42.18 | **41.47 (−0.91 %)** | 0.14 % |
| | `DynMeshes` RASTER ms | 1.23 | 1.20 | 0.77 (−37.4 %) | 3.33 % |
| **greets t=2845** | frame min ms | 50.55 | 50.72 | **49.78 (−1.52 %)** | 1.67 % |
| | `DynMeshes` bake ms | 1.72 | 1.77 | 0.88 | 44 % — do not quote |
| **city t=1961** (his arm) | frame min ms | 46.14 | 46.00 | 45.89 | 0.35 % |
| | `renderFrame` Ginstr/f | 4.176 | 4.176 | 4.176 | 0.02 % |
| **fountain t=2500** | frame min ms | 36.59 | 35.81 | 36.36 | 1.68 % |

**city and fountain bake NO shadow map at these poses — zero bake invocations —
so their rows are the inertness control, and they are inert.** So are chase and
crash: `--shadows` on any of the four produces 0 bakes. The shadow depth raster
runs at greets and at `render_gate.sh`'s `conetest`, and nowhere else in this
battery — which is the same fact §00e reported as "city t=1961: zero shadow
entries", now stated as a property of four scenes rather than one pose.

**The instrument's own cost (parent → child OFF) is null**: the frame minimum
moves −0.36 / +0.51 / −0.79 / +0.07 / +0.34 / −0.30 % across the six poses —
both signs, all within or beside the floor — and `renderFrame` Ginstr/f never
moves by more than one printed LSB. The OFF arm reproduces the parent's
`--clip_stats` census to the digit (8 499 390 / 6 995 771 / 5 702 196).

### THE SYMBOL, AFTER

`sample <pid> 25` on the running bench, self samples as a share of DEMO's, one
pose per process (`scratchpad/selftime2.sh` + `sumself.py`, §00e's instrument):

| symbol, greets t=5743 | parent | child |
|---|--:|--:|
| `FrustumClipper::Render` | **2.265 %** (§00e: 2.270 %) | **1.429 %** |
| `MekaleleShadowDepth` | 1.203 % | 1.159 % |
| `Transform_Objects` | 3.195 % | 3.349 % |

**greets' clipper row is now 1.43 %, against city's 1.28 % — the anomaly §00e
opened is closed.** What is left of it is no longer the shadow pass's 2-D
reject; it is §00e's own leftovers list (the Z clip at the top, then `YSort`,
the UV stamp's `Face` load, the double `stats_tls`), each below 0.5 % of frame
on its own. `MekaleleShadowDepth` barely moves, as it must: the rejected faces
never reached it.

### WHY IT IS BYTE-EXACT, AND THE PROBE THAT LOOKED FOR A COUNTER-EXAMPLE

The test is `RenderInner.cpp`'s verbatim — `bbMaxX < tx1 || bbMinX >= tx2 ||
bbMaxY < ty1 || bbMinY >= ty2` against the same rect the clipper was just given
by `SetClippingExtents`. §00e's caveat said the per-light clone FList "may not
stamp bboxes"; **it does.** The shadow FList is built by `Transform_Objects`,
whose stamp is gated only on `--tile_bbox_cull` (its `--xfrm_ablate` escape is
main-view-only: `xab = (_xablate != 0) && (xresOverride < 0)`, and the shadow
path always passes `xresOverride = sm.xres`), and the PX/PY it stamps are
SHADOW-MAP pixels because the light's `CameraContext` drives the projection. Box
and rect are in the same space; `bboxNearZ = cam.nearZ` is the same near plane
the clipper's `Near()` tests.

The S2 argument, re-derived for the light frustum:

* the box is a conservative superset of the un-clipped triangle (floor/ceil, 1 px
  margin, int16-SATURATED — saturation only ever widens it);
* it is a real box only when all three verts are in FRONT of the near plane, so
  the faces whose manufactured vertices could land outside it — the near-clipped
  ones — keep the cover-all sentinel and are never rejected. The guard is `z >
  nearZ`, strictly stronger than `Vtx_VisNear`'s test, so the equality edge falls
  on the safe side;
* the FAR clip DOES manufacture vertices here (the geometry straddles each
  light's range — that is §00e's `needZ`), but both endpoints of the interpolated
  edge are in front of near, so the projection of any point on the 3-D segment
  stays on the projected 2-D segment and therefore inside the box;
* the 2-D clip only shrinks coverage.

**And then it was searched for a counter-example instead of being trusted.**
`-DFDS_SHADOW_BBOX_VERIFY=ON` builds a binary whose walk computes the reject and
does NOT apply it, brackets every `clipper.Render` with a per-thread counter
bumped at the top of `MekaleleShadowDepth`, and reports any polygon the raster
received from a face the reject would have discarded:

| arm | rejectable face-visits that reached the clipper | **polygons they produced** |
|---|--:|--:|
| greets t=5743 / 2845 / 6097 / 6133 | 4.01 M / 4.05 M / 3.83 M / 3.96 M | **0 / 0 / 0 / 0** |
| greets t=1588 / 3122 / 4871 | 6.97 M / 2.56 M / 6.99 M | **0 / 0 / 0** |
| greets `--shadow-backface-cull` | 2.11 M | **0** |
| `FDS_SHADOW_TILE_GRID=8` | 43.1 M | **0** |
| `FDS_SHADOW_TILE_GRID=1` | **0** (the map-rect finding above) | 0 |
| `render_gate.sh conetest` | 5 952 | **0** |
| **total** | **79.7 M** | **0** |

### THE PLANE HASH — the gate this class of change actually needs

Shadow maps feed every scene's lighting, so a wrong reject reads as acne or
popping over TIME; four byte-identical snapshots do not prove the planes agree.
**`--shadow_plane_hash`** (new, default OFF, one bool load per bake invocation)
FNV-1a's the packed plane — depth AND polyId, every byte of every texel — of
every map a bake wrote, in the tick thread's deterministic map order, and prints
one `[SPH]` line per bake plus a running digest. Streams compared
parent-configuration to child, default vs `--no-shadow_bbox_cull`:

| arm | bakes | verdict |
|---|--:|---|
| greets t=5743 / 2845 / 6097 / 6133 / 1588 / 3122 / 4871, `--bench` iters=20 | 43 each | **IDENTICAL** |
| greets t=5743 snapshot; 640×360; no-displace; `--shadow-dynamic`; `--shadow-backface-cull` | 3–43 | **IDENTICAL** |
| greets t=6293,6294 with `FDS_GREETS_SHATTER=1` | 5 | **IDENTICAL** |
| `render_gate.sh conetest` | 12 | **IDENTICAL** |
| greets t=5743 under `--no-tile_bbox_cull` (cull inert) | 43 | **IDENTICAL** |
| city / chase / fountain / crash, with and without `--shadows` | **0 — they bake none** | — |

### GATES

* **11 pin recipes, 3/3 each, parent-binary-identical** (`DEMO_base` = tip
  `5071cc37` vs the child, one worktree, one asset tree): city `bd4ffbf8` /
  `4cb8d2ca` / `f473fe2b` (t=2400) / `d3374de6` (t=400), chase `3bfd4244` /
  `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain `8db68ccb` — **all
  ten at their recorded 16f values** — plus greets t=1588 and the four greets
  acceptance poses, differential. The `--no-shadow_bbox_cull` arm was spot-checked
  on city/greets/fountain, 3/3 at the same hashes.
* `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` /
  `166fa25a`). **`conetest` is the arm with teeth and it has them here**: its
  shadow clipper entries go **8 448 → 2 496 (−70.5 %)** and its reject rate
  **48.9 % → 6.7 %** while the rendered surface stays byte-identical.
* One render eyeballed per scene — `docs/img/shadowbbox/greets_t5743_shadowbbox.png`,
  `city_t1961_shadowbbox.png`, `chase_t800_shadowbbox.png`,
  `fountain_t2500_shadowbbox.png` — all correct.

---

## 00e. THE CLIPPER'S PER-FACE RESIDUE, PRICED — 2026-08-16p: the copy is 2.6 % of it, and the row §00c handed on is CLOSED BELOW BAR

**§00c closed row 3 and handed on one sentence: "`FrustumClipper::Render`'s
residue is the per-(face, tile) clip itself — three 140-byte `Vertex` copies,
the UV/UZ stamp, and `MiplevelClipper`'s subdivision … Cutting it further means
cutting the copy, not the traversal." That sentence is WRONG, and it is wrong by
a factor of 10 to 40.** The copies are **2.6 % of the clipper symbol's self time
at city t=1961 (0.033 % of frame)**; `MiplevelClipper` is another 0.13–0.17 %.
Together — the residue exactly as §00c named it — they price at **0.25 % of
frame (city t=1961), 0.25 % (chase t=800), 0.48 % (greets t=5743)**, below the
0.5 % bar at every acceptance pose. **All three candidate mechanisms are dead:
copy elision is structurally impossible, payload shrink is refuted by direct
measurement, and SIMD-ing a copy that costs 43 instructions has nothing to buy.**

Landed: the instrument only — **`--clip_stats`**, a per-(face, tile) census
split by dispatcher. Byte-null (11 pin recipes identical parent-to-child,
`render_gate.sh` 4/4).

### WHAT THE SYMBOL COSTS, AT EVERY POSE

`sample <pid> 25` on the running bench, leaf histogram, share of DEMO self
samples — the same unprivileged instrument and the same denominator §00c used
(`scratchpad/selftime2.sh`, which also handles chase's no-`--bench` case by
repeating one snapshot timestamp 200×). One pose per process, tip `dc752523`.
**`MiplevelClipper` does not appear as a symbol: it is inlined into `Render`, so
this one number IS the whole per-(face, tile) clip.**

| pose | `FrustumClipper::Render` self | DEMO self samples |
|---|--:|--:|
| city t=1961 (his arm) | **1.279 %** | 112 068 |
| city t=2400 | 1.360 % | 43 027 |
| city t=400 | 0.740 % | 65 791 |
| chase t=800 | 1.012 % | 16 692 |
| chase t=1600 | 0.847 % | 3 897 — thin, do not quote the LSB |
| **greets t=5743 (his arm)** | **2.270 %** | 116 992 |

§00c's 1.21 % at city t=1961 reproduces here at 1.279 %. **greets is the biggest,
at 2.27 %** — which §00c did not know, because the binning round measured greets
as neutral and stopped looking.

### THE SPLIT — two independent instruments, and they disagree about the copy in the way that decides the row

**(1) PC-offset histogram on the SHIPPING (inlined) binary.** `sample`'s call
tree carries `+offset [0xaddr]` on every frame; summing *self* samples (node
count − Σ children) by offset and mapping onto `otool -tvV`'s disassembly of
`FrustumClipper::Render` (9 512 bytes, 2 378 instructions) attributes the symbol
to source blocks with **no code change at all**. Landmarks that fix the block
boundaries, all verified in the disassembly rather than guessed: `clipperEntered++`
at `+76`, the three copies `+100..+284` (5 `ldr q`/`str q` pairs + one pointer
load per vertex = 43 instructions), the Face UV load `ldr d3, [x23, #0x30]` at
`+296`, `clipNeedZ++` at `+680`, `clipNeed2D++` at `+2136`, YSort's two
pointer-rotation loops `+3436..+3508`, `mipEntered++` at `+3556`.

**(2) a throwaway `__attribute__((noinline))` probe** on `YSort`,
`MiplevelClipper` and a `ProbeCopy3(F)` wrapper around the three copies, so each
becomes its own `sample` symbol and skid cannot move work across a call boundary.

| component | city t=1961 | chase t=800 | greets t=5743 |
|---|--:|--:|--:|
| `Render`, the rest (UV stamp, `Calc_Flags`, `Near`/`Far`, L/R/U/D) | 0.726 % | 0.638 % | **1.583 %** |
| `YSort` | 0.299 % | 0.238 % | 0.264 % |
| `MiplevelClipper` | 0.162 % | 0.169 % | 0.133 % |
| **the three `Vertex` copies** | **0.095 %** | **0.077 %** | **0.347 %** |
| `FInterpolator` | 0.045 % | 0.046 % | 0.044 % |
| `fds::stats_tls` (two calls per visit) | 0.039 % | 0.100 % | 0.087 % |
| sum | 1.366 % | 1.268 % | 2.458 % |

The two instruments bracket the copy: the noinline column **over-states** it
(forcing the copy out of line serialises three scattered vertex loads that the
inlined build overlaps with the UV stamp, and adds a call per visit), the offset
histogram on the shipping binary reads **0.033 % / 0.066 % / 0.009 %**. Take the
noinline figure as the ceiling. Either way the copy is **0.03–0.35 % of frame**,
and §00c's residue (copies + `MiplevelClipper`) is **0.25 / 0.25 / 0.48 %**.

**Read the greets column, because it is the one that matters.** greets' clipper
is 2.27 % and only 0.48 % of it is what §00c pointed at. Half of the rest —
1.16 % of frame by the offset histogram — sits in the Z-clip block `+660..+2128`,
and the census below says why: greets' clipper is 83 % SHADOW raster.

### MECHANISM (a), COPY ELISION: STRUCTURALLY IMPOSSIBLE, NOT MERELY UNPROFITABLE

The brief asked whether the clipper only READS the source vertices on the
no-clip path. It does not. `Render` writes to what it copies, twice over, and
both writes are the reason the copy exists:

* **per FACE** — `A->U = F->U1; A->UZ = F->U1 * aRZ;` (+ `EU/EV/EUZ/EVZ` under
  `Face_Reflective`). UVs live on the *face*, not the vertex, because one mesh
  vertex is shared by faces with different UVs. Stamping them into the mesh
  vertex would corrupt every other face that shares it.
* **per TILE** — `Calc_Flags(A/B/C)` recomputes `Vertex::Flags` against *this
  tile's* `ClipX1..ClipY2`. The same face is visited by 1.45 tiles on average
  (§00c), each wanting different flags.

And the two hazards compound: the tile jobs run on **12 pool workers at once**,
so any write to a shared mesh `Vertex` is a data race between tiles, not just a
correctness question within one. There is no "point until first mutation"
window — the first mutation is 4 instructions after the copy. The idea is dead
before the census is consulted; the census (below) only says how large the
population would have been.

### MECHANISM (b), SHRINK THE PAYLOAD: AUDITED SAFE, THEN REFUTED BY MEASUREMENT

A full field audit of every route into the clipper's copies — the clipper's own
`C_Prim`/`C_Scnd`/`_IA`/`_IB`/`newVert`, every `RasterFunc` (`MekaleleImpl<>` ×3,
all 15 `TheOtherBarry<>` instantiations, `MekaleleShadowDepth`), and the `vc[]`
re-copies inside them — says the copy can safely drop the **36-byte object-space
`Pos`/`N`/`Tangent` block at `[52, 88)`**: no reader anywhere reaches those
through a clipper copy. (`Mekalele.h:1016-1019` reads `F->B->Pos.x - F->A->Pos.x`
— that is the ORIGINAL mesh vertex through `F->A`, not the copy. `Vertex::i` is
likewise dead: `FRUSTRUM.CPP`'s `V->i = _IA->i` is the only read of that field in
the tree.) Two `memcpy`s instead of one assignment, 104 of 140 bytes.

Three binaries from one worktree, differing only in the copy width:

| arm | copy | correct? |
|---|--:|---|
| `cp0` | 140 B — `*A = *F->A` ×3, the shipping form | yes |
| `cp1` | **104 B** — `[0,52)` + `[88,140)`, the audited-safe cut | yes |
| `cp2` | 52 B — head only | **no** — a timing CEILING that removes 63 % of the payload |

**min-of-11, round 0 dropped, arm order ROTATED per round, one pose per process,
`--deferred_prof=4 --hw_prof`, 1512×848** (`scratchpad/ladder.sh`). Noise floor
per column = the largest `(2nd-min − min)/min` across the three arms; the
instruction columns are printed to 3–4 significant digits, so **one printed LSB
is 0.02 % on `renderFrame` and 0.10–0.30 % on `gbuffer`** — do not read below it.

| pose | column | cp0 (140 B) | cp1 (104 B) | cp2 (52 B) | noise floor | 1 LSB |
|---|---|--:|--:|--:|--:|--:|
| **city t=1961** | `renderFrame` Ginstr/f | 4.1760 | 4.1740 (−0.05 %) | 4.1730 (−0.07 %) | 0.05 % | 0.02 % |
| | `renderFrame` Gcyc/f | 1.1070 | 1.1060 (−0.09 %) | 1.1070 (0.00 %) | 0.27 % | 0.09 % |
| | `gbuffer` Ginstr/f | 0.4450 | 0.4450 (0.00 %) | 0.4440 (−0.22 %) | 0.00 % | 0.22 % |
| | frame min (ms) | 46.14 | 46.16 | 46.01 | 0.37 % | — |
| **greets t=5743** | `renderFrame` Ginstr/f | 4.6790 | 4.6800 (+0.02 %) | 4.6780 (−0.02 %) | 0.00 % | 0.02 % |
| | `renderFrame` Gcyc/f | 1.2720 | 1.2670 (−0.39 %) | 1.2690 (−0.24 %) | 0.32 % | 0.08 % |
| | `gbuffer` Ginstr/f | 0.9920 | 0.9920 (0.00 %) | 0.9910 (−0.10 %) | 0.00 % | 0.10 % |
| | frame min (ms) | 50.06 | 49.85 | 49.96 | 0.28 % | — |
| **chase t=800** | `renderFrame` Ginstr/f | 3.4670 | 3.4680 (+0.03 %) | 3.4660 (−0.03 %) | 0.00 % | 0.03 % |
| | `renderFrame` Gcyc/f | 0.7890 | 0.7840 (−0.63 %) | 0.7920 (+0.38 %) | 0.25 % | 0.13 % |
| | `gbuffer` Ginstr/f | 0.3350 | 0.3350 (0.00 %) | 0.3340 (−0.30 %) | 0.00 % | 0.30 % |
| | frame min (ms) | 43.39 | 43.42 | 42.74 | 1.99 % | — |

**Every INSTRUCTION reading is within one printed LSB** — `renderFrame` Ginstr/f
prints to 3 decimals, so one LSB is 0.024 %, and `gbuffer` Ginstr/f's is
0.10–0.30 %. **The cycle column moves further than that at chase, and in an
order that cannot be a mechanism:** `cp2` cuts 63 % of the payload where `cp1`
cuts 26 %, yet at chase `cp1` reads **−0.63 %** on `renderFrame` Gcyc and `cp2`
reads **+0.38 %**. A cut 2.4× larger cannot be 1.6× worse. The same inversion
appears at city (cp1 −0.09 %, cp2 0.00 %), and `cp1`'s `renderFrame` Ginstr
delta flips sign between city (−0.05 %) and greets/chase (+0.02 / +0.03 %).
That is the signature of noise, not of a mechanism.

**THE DISASSEMBLY SAYS THE SAME THING BEFORE THE STOPWATCH DOES, and this is the
number to quote.** The copy is 43 instructions. `cp1` removes one `ldr q` +
one `str q` per vertex (≈6 per face-visit); `cp2` removes ≈18. At the measured
entry counts:

| pose | visits/frame | `cp1` saves | `cp2` saves | as a share of `renderFrame` Ginstr/f |
|---|--:|--:|--:|--:|
| city t=1961 | 63 418 | 0.38 Minstr | 1.14 Minstr | **0.009 % / 0.027 %** of 4.176 G |
| greets t=5743 | 277 777 | 1.67 Minstr | 5.00 Minstr | **0.036 % / 0.107 %** of 4.679 G |

The whole mechanism is worth **three hundredths of one percent** at the pose it
was named for. It cannot be measured on this box because there is nothing there
to measure. **(c), SIMD-ing the copy, is the same arithmetic with a smaller
numerator** — `clang` already emits 128-bit `ldr q`/`str q` pairs for the
`pack(1)` 140-byte struct assignment, which is the widest load-store arm64 has,
so there is no wider form to reach for.

### THE CENSUS — `--clip_stats`, the instrument this round lands

`FrustumClipper::Render` bumps one indexed per-thread counter per visit (the
SAME single increment the flat counter cost, so tagging is free) and
`--clip_stats` prints an atexit table split by **which dispatcher** built the
clipper — `fds::ClipSrc`, set once per tile job, never per face. Per-frame
figures below are `(iters=28 − iters=8) / 20` on the same `--bench` arm, which
subtracts init, the env bake and the shadow prebake exactly.

**city t=1961, his arm — 63 418 clipper entries per frame:**

| dispatcher | entries/f | no-clip | emitted | rejected |
|---|--:|--:|--:|--:|
| `RenderInnerMekalele` (gbuffer) | 47 681 | 78.4 % | 18.3 % | 3.3 % |
| xpar strip raster (surface kernel) | 15 442 | 0.7 % | 97.1 % | 2.2 % |
| `RenderInner` (fwd, tile job) | 295 | — | — | — |
| `Shadows.cpp` depth raster | **0** | — | — | — |
| **all** | **63 418** | **59.2 %** | **37.8 %** | **3.0 %** |

city bakes no deferred shadow map at this pose, which is exactly why its clipper
is half greets'. The 2.69 M `RenderInner` entries the census reports are the env
cube bake and are constant in `iters` — they are init, not frame.

**greets t=5743, his arm — 277 777 clipper entries per frame, 4.4× city:**

| dispatcher | entries/f | no-clip | emitted | **rejected** |
|---|--:|--:|--:|--:|
| **`Shadows.cpp` depth raster** | **231 735 (83.4 %)** | 13.4 % | 4.8 % | **81.8 %** |
| `RenderInnerMekalele` (gbuffer) | 44 629 | 71.8 % | 26.7 % | 1.5 % |
| xpar strip raster (surface kernel) | 1 300 | 13.8 % | 50.8 % | 35.4 % |
| `MekaleleFillRegionInline` (RTT) | 113 | 21.2 % | 78.8 % | 0.0 % |
| **all** | **277 777** | **22.8 %** | **8.5 %** | **68.7 %** |

`rejected` = the clip threw the polygon away entirely (`C_numVerts == 0`): the
three copies and the UV stamp were paid and bought no pixels. **The whole
"greets' clipper is expensive" story is the shadow raster**, and 189 567 of its
231 735 entries per frame produce nothing. That is the row this one hands on —
see backlog **2026-08-16p**, not this section, for what to do about it.

Controls: `--no-tile_bbox_cull` takes greets' cumulative entries 8.50 M → 39.84 M
(the cull IS live, removing 79 %), and `--no-face_tile_bin` reproduces the
default count EXACTLY (8 499 390 both ways), which is the binning round's
order-preservation claim re-verified from a different direction.

### WHAT THE INSTRUMENT ITSELF COSTS — the OFF-arm control, because it lives in the hot path

`--clip_stats` puts a `FeatureFlags` bool load in `FrustumClipper::Render` on
every face-visit, and turns the flat `clipperEntered++` into an indexed
`clipperEntered[src]++`. Parent (`dc752523`, no instrument) vs child, one
worktree, min-of-11, order rotated:

| column | city t=1961 | greets t=5743 | noise floor |
|---|--:|--:|--:|
| `renderFrame` Ginstr/f | 4.1740 → 4.1750 (**+0.02 %**) | 4.6800 → 4.6810 (**+0.02 %**) | 0.02–0.05 % |
| `renderFrame` Gcyc/f | 1.1090 → 1.1120 (+0.27 %) | 1.2760 → 1.2720 (−0.31 %) | 0.08–0.18 % |
| `gbuffer` Ginstr/f | 0.4440 → 0.4450 | 0.9910 → 0.9920 | 1 LSB |
| frame min | 46.18 → 46.23 | 50.16 → 50.66 | 0.26–0.35 % |

**+1 printed LSB on instructions at both poses; the cycle column changes sign
between them.** That is the floor, not a cost.

### WHAT `render_gate.sh` ACTUALLY EXERCISES HERE — measured, not assumed

4/4 PASS is worth stating precisely, because "the gate passed" says nothing
about a clipper change unless the gate runs the clipper. Each arm re-run with
`--clip_stats`:

| gate arm | clipper entries | buckets it reaches |
|---|--:|---|
| `mirrortest` | 3 872 | `DeferredTiled` 3 440, `DeferredXpar` 432 |
| `rttslot` | 3 947 | `DeferredTiled` 3 612, `DeferredXpar` 324, `DeferredInline` 11 |
| `conetest` | 13 764 | `DeferredTiled` 5 316, **`ShadowMap` 8 448** |
| `halotest` | 274 | `DeferredTiled` 274 |

**21 857 clipper entries across the four arms, reaching 4 of the 7 `ClipSrc`
buckets.** `conetest` is the only arm that rasterises a shadow map at all, and
it does so at a **48.9 % reject rate** — a small-scale replica of the greets
finding below. `ForwardTiled`, `ForwardInline` and `DeferredStrip` are NOT
covered by the gate; the city / chase / greets pin recipes cover the first and
the third (city's xpar strip raster alone is 15 442 entries/frame), and no gate
or pin in this round exercises `ForwardInline` (the mirror-shard forward bake).

### WHAT THE REST OF THE SYMBOL IS, FOR WHOEVER COMES BACK

Ranked by the noinline probe, the parts of the clipper that are NOT the copy and
NOT `MiplevelClipper`:

1. **the inlined Z clip `Near()`/`Far()`** — 1.164 % of frame at greets t=5743
   by the offset histogram (51 % of the symbol), against 0.115 % at city. It runs
   on only 5.2 % of greets' entries, so this is a per-call cost (a ~20-field
   lerp plus `Calc_Flags` per manufactured vertex), and it is concentrated in the
   shadow pass, whose light frusta the scene geometry straddles.
2. **`YSort`** — 0.24–0.30 % of frame, stable across all three poses. A pointer
   rotation over ≤7 entries with two variable-trip loops; `nVerts == 3` is the
   overwhelming case and a branchless 3-way specialisation would be a pure
   permutation, i.e. trivially bit-exact. Below bar on its own.
3. **the UV stamp's first Face load** (`+296`, `ldr d3, [x23, #0x30]`) — the
   single hottest instruction in the symbol at city (13.2 % of its self time).
   It is a cache miss on the `Face`, not arithmetic.
4. **two `fds::stats_tls()` calls per visit** — 0.04–0.10 % of frame. `Render`
   captures the block once (`FDS_STATS_SCOPE`) and `MiplevelClipper` captures it
   AGAIN because it is a separate function even when inlined, so the opaque
   cross-TU call is issued 107 122 times per frame at city. Bit-exact to remove
   (pass the captured reference down). Below bar.

None of these clears 0.5 % on its own at more than one pose. The clipper is not
one expensive thing any more; it is six cheap ones.

---

## 00d. THE FINER RASTER GRID, RE-RUN — 2026-08-16d: §00 row 9's refutation is dead, and the winning shape is not the one that was tried

**§00 row 9 refuted a finer frame raster grid at "+41 % wall, +139 % instructions"
because each raster tile re-walked the whole face list. `c26c1c35` +
`d9dfa527` removed that traversal (§00c), so the refutation was re-run from
scratch. Two things came out of it, and the second one is the result:**

1. **The stated cost is gone.** The identical 12×10 probe now costs **+2.7 %**
   `gbuffer` instructions at chase t=800, not +139 %. Nobody should quote row 9
   as a reason again — but 12×10 also **buys almost nothing** (`effPar`
   2.9 → 3.4), so the shape row 9 tried is simply not the interesting one.
2. **The win is entirely in the Y axis, and it is worth −11.7 % of chase's
   `renderFrame`.** At a FIXED 120 tiles, `6×20` beats `12×10` by 35 % of the
   `gbuffer` wall and beats `24×5` by 33 %. Shape dominates count.

Landed as **`--frame_tile_x` / `--frame_tile_y`, DEFAULT 6×5 = the historical
constant.** The default path is byte-null (8/8 pins identical to the parent
binary, `render_gate.sh` 4/4) and **nothing is switched on**. What follows is
the evidence for a judge call, not a change that was made.

### THE COUNTER COLUMN — what row 9's mechanism actually costs today

chase t=800, 1920×1080, snapshot harness, `--deferred_prof=1 --hw_prof`,
min-of-13 over 14 interleaved rotated rounds.

| | row 9 (2026-08-14) | today, 6×5 | today, 12×10 |
|---|--:|--:|--:|
| `gbuffer` Ginstr/f | 0.597 → **1.424 (+139 %)** | 0.334 | **0.343 (+2.7 %)** |
| `gbuffer` thrsum (core-ms) | 57.2 → **141.9 (+148 %)** | 25.5 | **28.0 (+9.8 %)** |
| `gbuffer` wall | 10.22 → 14.41 (+41 %) | 8.50 | 8.05 |
| `gbuffer` `effPar` | 5.2 → 9.1 | 2.9 | 3.4 |

Note the 6×5 column has itself moved since row 9 — `effPar` reads **2.9, not
5.5**, because §00c took 57.2 → 25.5 core-ms out of the phase without moving the
barrier. The idle fraction got *worse* as the work got cheaper, which is why the
question was worth re-asking.

### THE SHAPE SWEEP — at a fixed tile count, the aspect ratio decides

chase t=800, same harness, min-of-11 over 12 rotated rounds, `gbuffer` wall / its
`Ginstr/f`:

| grid | tiles | `gbuffer` wall | `effPar` | `gbuffer` Ginstr/f | `renderFrame` wall |
|---|--:|--:|--:|--:|--:|
| **6×5 (default)** | 30 | **8.50** | 2.9 | 0.334 | **33.69** |
| 12×10 | 120 | 8.05 | 3.4 | 0.343 | 32.83 |
| 24×5 | 120 | 7.78 | — | 0.364 | 32.57 |
| **6×20** | **120** | **5.22 (−38.5 %)** | **5.1** | **0.350 (+4.8 %)** | **29.75 (−11.7 %)** |
| 6×16 | 96 | 8.60 | 3.0 | — | 32.79 |
| 16×13 | 208 | 6.04 | 4.7 | 0.354 | 30.69 |
| 12×20 | 240 | 5.82 | — | 0.360 | 30.64 |
| 24×10 | 240 | 7.16 | — | 0.365 | 31.68 |
| 24×20 | 480 | 5.19 | 6.0 | 0.384 (+15 %) | 30.10 |

**Three tilings of 120 tiles land 35 % apart on wall.** `6×20` gets the whole
win of `24×20` at a third of its extra instructions, and `6×16` — four fewer
rows — gets none of it. `24×5` (all the extra tiles spent on columns) costs
nearly twice `6×20`'s instructions for a quarter of its win: chase's geometry is
wide and short, so an extra COLUMN re-clips faces that already spanned the row
while an extra ROW splits them.

**The 16→20 row cliff is NOT explained.** A plausible mechanism — with 6 columns
and 12 workers the pool covers two tile ROWS per wave, averaging two screen bands
instead of one — was not tested. Recorded as an open question, not a finding.

### ACROSS THE ACCEPTANCE ARMS — `6×20` vs the default

Clean-load window (load 3.0 → 17), min-of-11 over 12 rotated rounds, one pose per
process, 1512×848 for the bench poses. Arms: city `--env_live_water --deferred
--city_env_pixel`; greets `--deferred --hdr --hdr-linear --texture-filter=2
--ssao --ssao-gtao --greets-displace`; chase/fountain as §00.
`par` = the PARENT binary at 6×5, the control that prices the runtime-grid
refactor itself.

| pose | number | par (6×5) | 6×5 | **6×20** | 24×20 |
|---|---|--:|--:|--:|--:|
| **chase t=800** | `renderFrame` wall | 32.96 | 33.69 | **29.75 (−11.7 %)** | 30.10 |
| chase t=1600 | `renderFrame` wall | 17.36 | 17.28 | **16.57 (−4.1 %)** | 16.66 |
| **city t=1961** | frame min | 47.67 | 47.49 | **46.63 (−1.8 %)** | 47.48 |
| **greets t=5743** | frame min | 54.34 | 54.02 | **54.23 (+0.4 %)** | 56.08 (+3.8 %) |
| fountain t=2500 | frame min | 36.19 | 37.56 | 36.39 | 36.67 |

`par` vs `6×5` is the noise floor: they are the same code and read 2.2 % apart on
chase and 3.8 % apart on fountain, so **fountain's column says nothing** and
chase's −11.7 % / city's −1.8 % are outside it.

Instruction cost of `6×20`, frame-wide (`renderFrame` Ginstr/f):
**chase +0.5 %, city +1.5 %, fountain +2.0 %, greets +2.1 %** — the `gbuffer`
row pays +4.8 / +13.5 / +11.7 / +10.6 % respectively. `--face_tile_bin`'s arena
grows 403 KiB → 851 KiB at city (1.22 MiB at 24×20).

### THE BYTES — and this is what decides it

Tile boundaries move, so every face is clipped against different rects and
`MiplevelClipper` subdivides a different sub-polygon. Diff of the DEFAULT grid
against `6×20`, at each pin's acceptance arm, 1920×1080:

| pin | changed px | % | max \|Δ\| | mean \|Δ\| over changed | localisation |
|---|--:|--:|--:|--:|---|
| chase t=100 | 58 | 0.003 % | 71 | 4.5 | scattered |
| chase t=400 | 562 | 0.027 % | 73 | 7.0 | **seam: 4.8× enriched in ±8 px of a 6×5 boundary** |
| chase t=800 | 337 | 0.016 % | 33 | 3.0 | **seam: 2.3× enriched in ±8 px of a 6×20 boundary** |
| chase t=1200 | 1 738 | 0.084 % | 190 | 1.6 | 1 673 of 1 738 are \|Δ\|=1 |
| chase t=1600 | 59 | 0.003 % | 52 | 3.0 | scattered |
| fountain t=2500 | 810 | 0.039 % | 231 | 13.3 | 1.6× enriched at seams |
| **city t=1961** | **38 430** | **1.85 %** | 160 | 2.2 | **NOT seam-local (1.0–1.2× — i.e. uniform)** |
| **greets t=5743** | **339 472** | **16.37 %** | 122 | 4.8 | **NOT seam-local (1.1–1.4×)** |

**greets is the veto case and its mechanism is measured, not guessed.** Re-running
the same 6×5-vs-6×20 diff with single flags removed:

| greets arm | changed px | % |
|---|--:|--:|
| his full arm | 339 472 | 16.37 % |
| `--texture-filter=0` instead of 2 | 144 319 | **6.96 %** |
| without `--ssao --ssao-gtao` | 347 100 | 16.74 % |
| without `--greets_displace` | 92 527 | **4.46 %** |

Trilinear filtering carries ~58 % of it and displacement most of the rest: both
make the mip level a function of the clipped sub-polygon, so a different tiling
re-selects mips over whole SURFACES, not just at seams. SSAO is not involved.
**greets moves a sixth of the frame for +0.4 % wall — there is no trade there.**

Images (before / after / amplified diff), full paths:
`docs/img/tilegrid/{chase_t800,chase_t400,city_t1961,greets_t5743,fount_t2500}_6x5_before.png`,
`…_6x20_after.png`, `…_6x20_diff.png`.

### THE JUDGE CALL — Gil-Ad's, not taken here

The flag is in at the historical default and **the shipping look is unchanged**.
The recommendation, in order of confidence:

* **chase is a real, cheap win** — −11.7 % of `renderFrame` at t=800 and −4.1 %
  at t=1600 for +0.5 % instructions, and its bytes move by 58–1 738 px at max
  \|Δ\| ≤ 190 (mostly \|Δ\|=1), seam-local where they are enriched at all. If
  the chase pins are acceptable at that scale, `FF::setDefault(frame_tile_y, 20)`
  in `CHASE.CPP` is the whole change. **Per-scene, not global.**
* **greets: no.** Flat wall, 16 % of pixels moved. Do not enable it there.
* **city: marginal.** −1.8 % frame min is real but 1.85 % of pixels move and the
  change is uniform, not seam-local. Not worth it without a look review.
* **A GLOBAL default change is NOT recommended** on this evidence.

### WHAT THE CENSUS FOUND (frame grid vs the other three)

`--frame_tile_x/y` moves ONLY `RENDER.CPP:renderFrame`'s tiler, what
`--face_tile_bin` bins into, and the bound of the transparent peel's per-batch
composite. It cannot reach:

| grid | where | value | coupling to the frame grid |
|---|---|---|---|
| deferred LIGHTING | `DeferredCommon.h:76` `DEFERRED_NUM_TILES_X/Y` | 12×8 | none — `ctx.lt{NumX,NumY,SizeX,SizeY}` are set from THIS grid (`DeferredSurfaceKernel.cpp:7107`) and travel with the light array |
| TBR strip lights | `DeferredSurfaceKernel.cpp:4865` | 1 × (YRes/8) | none — same mechanism, set from the strip geometry |
| SSAO | `DeferredSSAO.cpp:228` etc. (5 sites) | 12×8 | none, own constant |
| fast fog | `DeferredFastFog.cpp:2742` etc. (6 sites) | 12×8 | none, own constant |
| shadow maps | `Shadows.cpp:726` | 4×4 | none, own constant |
| volumetric cones | `--cone_fine_tiles` | 12×8 / 6×4 | none |

The ONE coupling is the **legacy `--no-xpar_tile_lights` fallback**
(`DeferredSurfaceKernel.cpp:3735`), which subscripts the 96-entry light array
with the frame-tile ordinal and clamps: past 96 frame tiles every further tile
collapses onto light list 95. That arm is already the known-wrong one (the fix is
default ON); the clamp means a fine grid cannot read out of bounds.

Things that are NOT hardcoded and were checked: the peel-floor restore is sized
by `g_xparZCount` (the plane's own length, `RENDER.CPP:875`), not by tiles; the
xpar strip path is 8-row bands off `DEFERRED_MAX_STRIPS`; `ClipperTileRect` is
derived from the viewport, so it follows any tiling. The 168-row figure that
appears in `DeferredSurfaceKernel.cpp:3724` is a WORKED EXAMPLE of the
`(848+4)/5 & ~7` rule at 1512×848, not a constant — it is now labelled as the
default.

**Stale docs corrected in this round:** `ENGINE.md` said the raster grid was 6×4
= 24 jobs (it has been 6×5 = 30, dispatched by `dispatchIndexed`, not one enqueue
per tile); `GRAPHICS_PIPELINE.md` §6's canonical post-pass template said 6×4 with
a per-tile `enqueue` loop (the real passes are 12×8 via `dispatchIndexed`).

---

## 00c. THE G-BUFFER FILL — 2026-08-16c: the mirror pass had no tile cull at all

**§00b row 3 (`FrustumClipper::Render`, 6.2 %) is closed, and its stated
mechanism was wrong.** The row read "every raster tile re-walks the whole face
list"; the walk turned out to be the cheap half. What cost the frame was that
`--tile_bbox_cull` — default ON since the S2/B5 work — was **completely inert
on the mirror pass**, so 30 tiles × ~20 700 faces reached the clipper where
~30 000 (face, tile) pairs actually exist.

### THE CENSUS THAT SPLIT IT (city t=1961, 1512×848, 6×5 tiles)

`Transform_Objects` stamps each pushed face's projected screen bbox into its
`FListEntry`. The two hand-written mirror transforms —
`Reflected_Transform` in `DEMO/CITY.CPP` and `DEMO/CHASE.CPP`, which build
FList themselves for the water-reflection underlay — pushed
`*Ins++ = { F->SortZ.DW, F };`, a two-field aggregate that leaves the rest on
their default member initialisers, i.e. the **cover-all sentinel**:

| pass of the frame | cover-all entries | avg tiles/face | (face, tile) pairs |
|---|--:|--:|--:|
| main (`Transform_Objects`) | 1.2 % | 1.45 | 29 671 |
| mirror (`Reflected_Transform`) | **100 %** | **30.00** | **621 180** |

City runs both every frame (`renderFrame` calls/f = 2). chase has the same
transform and the same hole.

### WHAT LANDED

1. **`c26c1c35` — stamp the bbox on the mirror pushes**
   (`fds::FaceBBox_Stamp`, `FDS/Base/FaceBBox.h`). This is the whole prize.
2. **`d9dfa527` — `--face_tile_bin`** (default ON): `renderFrame` walks FList
   twice (count, scatter) and hands each tile a dense `Face*` run, so no tile
   walks the list at all. Order inside a tile is preserved exactly (chunk-major
   prefix sum over a contiguous split), verified element-by-element by a
   throwaway `FDS_BINVERIFY` build over 7 poses — 0 mismatches. New `face-bin`
   phase row prices the build at **0.200 ms/frame** at city t=1961;
   `--mem_census` reports 403 KiB arena + 113 KiB scratch there.

### MEASURED, parent `b2de6323` → `d9dfa527`

Two binaries in ONE worktree against ONE asset tree, interleaved, min-of-arm
with r0 dropped, 1512×848. **The box ran at load 17–24 for this batch, so the
wall columns below are floors, not clean numbers — `Ginstr/f` and `Gcyc/f` are
what decide** (the same rule §00b was measured under).

**CITY ARM** (`--env_live_water --deferred --city_env_pixel`):

| row | t=1961 (min-of-8) | t=2400 (min-of-5) | t=400 (min-of-5) |
|---|--:|--:|--:|
| **`gbuffer` Ginstr/f** | **0.658 → 0.444 (−32.5 %)** | 0.185 → 0.175 (−5.4 %) | 0.359 → 0.318 (−11.4 %) |
| `gbuffer` thrsum (core-ms) | 79.3 → 43.1 (−45.7 %) | 17.42 → 17.36 | 35.3 → 31.8 (−10.0 %) |
| `gbuffer` wall | 8.69 → 6.86 (−21.0 %) | 5.37 → 5.76 † | 4.00 → 3.53 (−11.9 %) |
| **`renderFrame` Ginstr/f** | **4.383 → 4.174 (−4.8 %)** | 2.273 → 2.269 | 3.216 → 3.180 (−1.1 %) |
| `renderFrame` Gcyc/f | 1.196 → 1.115 (−6.8 %) | 0.623 → 0.625 | 0.864 → 0.851 (−1.5 %) |

**GREETS ARM** (`--deferred --hdr --hdr-linear --texture-filter=2 --ssao
--ssao-gtao --greets-displace`):

| row | t=2845 (min-of-4) | t=5743 (min-of-8) | t=6097 (min-of-2) |
|---|--:|--:|--:|
| `gbuffer` Ginstr/f | 0.978 → 0.968 (−1.0 %) | 1.001 → 0.991 (−1.0 %) | 0.793 → 0.788 (−0.6 %) |
| `gbuffer` thrsum (core-ms) | 102.0 → 92.6 (−9.2 %) | 96.7 → 94.8 (−2.0 %) | 97.2 → 86.0 (−11.6 %) |
| `gbuffer` wall | 9.17 → 8.68 | 8.81 → 8.69 | 7.87 → 7.53 |
| `renderFrame` Ginstr/f | 4.983 → 4.981 | 4.996 → 4.990 | 4.405 → 4.406 |
| `renderFrame` Gcyc/f | 1.351 → 1.362 | 1.369 → 1.347 | 1.207 → 1.220 |

† t=2400's `gbuffer` wall is the only row that reads worse, at flat
instructions (−0.2 % of `renderFrame`) and flat thrsum (−0.3 %) — a 5.4/5.8 ms
wall pair under load 17–24, not a mechanism. The pose barely has the effect to
begin with.

### THE ROW ITSELF — sampled self time, city t=1961, his arm

`sample <pid> 25` on the running bench (leaf histogram; unprivileged, no
Instruments needed — `scratchpad/selftime.sh`), shares of DEMO self samples.
**The parent column reproduces §00b's 6.2 % at 6.46 %**, which is what makes the
after column quotable:

| symbol | parent | tip | absolute samples |
|---|--:|--:|---|
| **`FrustumClipper::Render`** | **6.46 %** | **1.21 %** | 9 223 → 1 644 (**−82.2 %**) |
| `RenderInnerMekalele` (the per-tile walk) | 2.53 % | 0.16 % | 3 605 → 217 (−94.0 %) |
| `fds::FaceTileBins_Build` + its dispatch | — | 0.10 % | 0 → 133 (new) |
| **the three together** | **8.99 %** | **1.47 %** | |
| `Render_VolumetricCones_Tile` (control) | 25.13 % | 27.16 % | 35 850 → 36 843 |
| `apply_exact<false>` (control) | 6.13 % | 6.23 % | 8 749 → 8 453 |

Both controls are flat in absolute samples — the shares only move because the
denominator shrank — so the two rows that did move, moved.

Note what the walk row says: at 2.53 % it was **not** negligible, and replacing
it with a 0.10 % build is a 27× cut in traversal. But it was 39 % the size of
the clipper row it was being blamed for; the clipper's own 6.46 % came from
being CALLED 21× too often on the mirror pass, which is why `c26c1c35` is the
commit that carries this round and `d9dfa527` is the smaller half.

**SPOT CHECKS — and chase is the biggest winner of the whole round**, because it
runs the same `Reflected_Transform` and its mirror frustum is full. chase t=800
(`--deferred`, profiled through the snapshot harness at one repeated timestamp,
min-of-5); fountain t=2500 (`--deferred --hdr --glass-refract=1 --glass-test`,
min-of-5, binning only — it has no mirror transform):

| row | chase t=800 | fountain t=2500 |
|---|--:|--:|
| **`gbuffer` Ginstr/f** | **0.597 → 0.334 (−44.1 %)** | 0.136 → 0.128 (−5.9 %) |
| `gbuffer` thrsum (core-ms) | 60.5 → 26.1 (**−56.9 %**) | 12.34 → 11.36 (−7.9 %) |
| `gbuffer` wall | 11.07 → 8.53 (−22.9 %) | 2.73 → 2.78 |
| **`renderFrame` Ginstr/f** | **3.729 → 3.475 (−6.8 %)** | 1.030 → 1.024 (−0.6 %) |
| `renderFrame` Gcyc/f | 0.894 → 0.815 (−8.8 %) | 0.409 → 0.408 |
| `renderFrame` wall | 38.43 → 36.09 (−6.1 %) | 34.49 → 34.21 |
| frame min | — (no `--bench` arm) | 37.40 → 36.95 (−1.2 %) |

§00 row 9 measured chase's `gbuffer` at `effPar` **5.5 of 12** and called it "half
the pool is idle". Half of what the pool was busy WITH is now gone: 60.5 → 26.1
core-ms at the same pose.

**Two things the city/greets table says, and both are the mechanism confirming
itself.**
t=2400 and t=400 move far less than t=1961: the mirror frustum there holds far
less of the scene-sized building, exactly the geometry-dependence that made
2026-08-16b's `LGHT` finding move only at t=1961. And **greets is NEUTRAL**, not
a win — it has no `Reflected_Transform` (its mirror is the RTT), so it gets only
the binning half: `renderFrame` instructions flat to −0.1 %, cycles ±1.6 %,
`gbuffer` instructions −1.0 %. Do not quote greets as a gain.

### THE WALL NUMBER, MEASURED PROPERLY

The tables above ran A-then-B in every round, which under this box's structured
load systematically penalises whichever arm runs second. Re-run with the arm
order ROTATED per round (`scratchpad/ab2.sh`), 13 rounds, r0 dropped — this is
the row to quote for city:

| city t=1961, his arm | parent `b2de6323` | tip `d9dfa527` | |
|---|--:|--:|--:|
| **frame min** | 50.61 | **47.74** | **−5.7 %** |
| **frame mean (TOTL)** | 57.71 | **54.24** | −6.0 % |
| `renderFrame` wall | 40.47 | 37.96 | −6.2 % |
| `gbuffer` wall | 8.571 | 6.023 | **−29.7 %** |
| `gbuffer` thrsum | 81.71 | 39.24 | **−52.0 %** |
| `gbuffer` Ginstr/f | 0.658 | 0.443 | −32.7 % |
| `renderFrame` Ginstr/f | 4.384 | 4.173 | −4.8 % |
| `renderFrame` Gcyc/f | 1.165 | 1.083 | −7.0 % |

Same treatment on greets t=5743 (12 rounds, rotated): **frame min 56.98 → 57.09
(+0.2 %), frame mean 85.48 → 83.57 (−2.2 %), `renderFrame` 50.13 → 48.06,
`gbuffer` 9.455 → 8.841 (−6.5 %), thrsum 98.3 → 92.5 (−5.9 %)** at
`renderFrame` **−0.1 % instructions / +0.1 % cycles**. Flat frame, as the
neutral verdict above says — and note that without the rotation the same pose
read +10.5 % on frame min at those same flat counters. That swing is the box,
not the change; do not read either sign of it as a result.

**BYTE-NULL** — all nine pins unmoved 2/2 on each binary, `render_gate.sh` 4/4.
The S2 contract is why: the box is a conservative superset of the un-clipped
triangle and the clipper only shrinks coverage, so a box that misses a tile
means zero output there. The escape hatches agree too — city t=1961 under his
arm gives `925ecd43…` on all four of default / `--no-face_tile_bin` /
`--no-tile_bbox_cull` / both off.

> **2026-08-16f — the `925ecd43…` here is HUD-BEARING and has been superseded.**
> It was taken without `--profiler=0`, and `RunCitySnapshot` did not silence the
> cfg-seeded profiler overlay, so the value carries 3 718 px of glyph text. The
> byte-null verdict above is unaffected (both arms of this A/B carried the same
> HUD). Current city his-arm pin: **`4cb8d2ca68b72f8a24627f42077eef25`**; see the
> 2026-08-16f block at the top of `docs/SESSION_STATE.md`.

### WHAT THIS UNBLOCKS

§00 row 9's refuted finer-grid experiment ("a 12×10 grid cost +139 %
instructions, because each clipper tile re-walks the whole face list") was
blocked on exactly the traversal that is now gone. It is worth re-running on
chase, whose `gbuffer` `effPar` is 5.5 of 12 — but note it moves tile
boundaries, so unlike everything above it will not be byte-null.

> **DONE — §00d (2026-08-16d).** Both halves of that paragraph came out true and
> one came out incomplete: the refutation is dead (12×10 now costs +2.7 %
> instructions, not +139 %), it is NOT byte-null (greets moves 16.4 % of its
> pixels), and the shape that wins is **6×20, not 12×10** — the same tile count
> spent on rows instead of a square, worth −11.7 % of chase's `renderFrame`.
> Landed as `--frame_tile_x/y` at the historical 6×5 default; the enable is a
> judge call, per scene.

---

## 00b. THE USER'S ACCEPTANCE ARM — city, 1512x848, 2026-08-16

```
./DEMO --env_live_water --deferred --city-env-pixel
```

**Nobody had ever profiled these two flags together**, and the arm is not a
small perturbation of §00's city rows — it moves the glass out of the forward
renderer and into the deferred kernel, and it turns on a per-pixel path in
`--env_live_water` that had only ever been priced on the FORWARD one. Poses are
the scene's own scripted camera at **t=1961 / 2400 / 400**, `iters=30`,
`--profiler=1 --deferred_prof=5 --hw_prof`, **min-of-10 over 11 interleaved
rounds** with round 0 dropped, two binaries in ONE worktree against ONE asset
tree. Loads 12–39 across the session — read `Ginstr/f` when a wall figure looks
surprising.

### TWO THINGS THE ARM INVERTS

* **`--city_env_pixel` makes city FASTER.** It moves the window glass into the
  G-buffer (`lighting-w1` 0.715 → 0.966 Ginstr/f at t=1961) and deletes the
  forward per-vertex reflective path, which cost more: clean single runs give
  frame min **77.11 ms** for plain `--deferred` against **59.22 ms** with the
  flag. Any city number quoted against plain `--deferred` is a slower frame
  than the one he runs.
* **`--env_live_water` is NOT free per pixel here.** The 2026-08-13b "free at
  the noise floor" was the FORWARD paraboloid path, per vertex. Through the
  deferred compose it is **+0.041 / +0.015 / +0.047 Ginstr/f of `lighting-w1`**
  at t=1961 / 2400 / 400 = **+4.4 % / +3.8 % / +6.3 %** of that phase, ≈ 0.74 ms
  at t=1961. The cost is the wave-slope evaluation the tilt calls per glass
  pixel, not the mask read.

Three-arm ablation of `lighting-w1` at t=1961 (Ginstr/f), which is how those
numbers were separated: plain `--deferred` **0.715** → + glass in the G-buffer
(`--city_env_pixel --no-env_refl`) **0.844** → + the env compose **0.933** → +
`--env_live_water` **0.974**.

### BEFORE / AFTER — his exact command, parent `44c8aeed` vs `545e96d9`

| | t=1961 par | t=1961 **new** | t=2400 par | t=2400 **new** | t=400 par | t=400 **new** |
|---|--:|--:|--:|--:|--:|--:|
| **frame min** | 54.720 | **49.760** | 30.950 | 30.890 | 36.360 | 35.990 |
| **frame mean (TOTL)** | 59.618 | **54.196** | 35.365 | 35.105 | 39.877 | 39.794 |
| **`LGHT` p50** | **5.904** | **0.974** | 0.189 | 0.164 | 0.281 | 0.266 |
| `renderFrame` (×2) | 40.011 | 40.133 | 24.149 | 24.043 | 28.425 | 28.164 |
| `cones-call` (×1) | 9.436 | 9.378 | 5.133 | 5.157 | 6.517 | 6.477 |
| `gbuffer` (×2) | 8.207 | 8.381 | 5.167 | 5.102 | 3.663 | 3.634 |
| `DeferredLighting-call` (×2) | 8.133 | 8.128 | 3.863 | 3.762 | 6.455 | 6.346 |
| `fastfog` (×1) | 7.524 | 7.452 | 4.148 | 4.106 | 4.631 | 4.530 |
| ⤷ `fog-columns` | 3.636 | 3.641 | 1.218 | 1.214 | 1.500 | 1.509 |
| ⤷ `fog-composite` | 3.111 | 3.004 | 2.398 | 2.309 | 2.639 | 2.591 |
| `TBR-render` (×2) | 4.200 | 4.168 | 3.264 | 3.269 | 4.617 | 4.627 |
| `water-glints` | 2.459 | 2.416 | 2.003 | 1.973 | 2.551 | 2.530 |
| `water-ripple` | 1.986 | 1.847 | 1.560 | 1.467 | 2.327 | 2.155 |
| `gbuf-clear` | 0.556 | 0.543 | 0.553 | 0.568 | 0.547 | 0.528 |
| **`Ginstr/f` `renderFrame`** | 4.409 | **4.387** | 2.288 | **2.278** | 3.243 | **3.222** |
| `Ginstr/f` `water-ripple` | 0.252 | **0.221** | 0.205 | **0.181** | 0.295 | **0.259** |
| `Ginstr/f` `water-glints` | 0.248 | **0.230** | 0.196 | **0.182** | 0.258 | **0.241** |
| `Ginstr/f` `fog-composite` | 0.392 | **0.377** | 0.311 | **0.301** | 0.347 | **0.335** |
| `Ginstr/f` `lighting-w1` | 0.963 | **0.957** | 0.391 | **0.389** | 0.736 | **0.728** |
| `Ginstr/f` `cones-call` | 1.288 | 1.288 | 0.686 | 0.686 | 0.894 | 0.894 |

**−4.96 ms of frame min and −5.42 ms of frame mean at t=1961 (−9.1 %), and
almost all of it is ONE row that had no instrument.** t=2400 and t=400 barely
move, and that is the mechanism confirming itself — see `LGHT` below.

### `LGHT` — the biggest WALL row in the frame and the smallest CPU row

`LGHT` runs OUTSIDE `renderFrame`, so no `--deferred_prof` table ever carried
it and no round of this campaign had opened it. Under this arm at t=1961 it
reads **5.9 ms p50** — ~10 % of the frame — while `LightMeshVerts` is **1.67 %
of DEMO self time** in a frame-dominated Time Profiler, i.e. ~9 CORE-ms. Wall 6
against 9 core-ms over 12 workers is `effPar` ≈ 1.5: a LOAD-IMBALANCE row, not
a compute row. The fan's unit was a MESH and city is 56 visible meshes of which
one is the scene-sized building; a work-stealing cursor cannot split a task.
Chunked to 1 024-vertex ranges it is **0.97 ms**, byte-null. t=2400/t=400 do not
have that mesh flagged visible, so they had no imbalance and gain nothing —
which is the check that the diagnosis is right.

`--prof_no_vertex_light` (default off, byte-null) is the ceiling instrument this
row needed; it is MEASUREMENT ONLY and changes pixels, because the forward
`TheOtherBarry` filler reads `Vertex::L{R,G,B}` for transparents, water, sprites
and the forward mirror-pass glass.

### INIT — `--env_live_water` bypasses `city_envmap_cache` BY DESIGN

The mask is a product of the bake's depth buffer, so a colour-only cache hit
would leave the tilt silently inert. Whole-process wall, `iters=5` (init
dominates), 3 runs each, all within 0.01 s:

| arm | process wall |
|---|--:|
| `--city_env_pixel`, cache warm | **0.98 s** |
| `--city_env_pixel --no-city_envmap_cache` (cold bake) | 2.18 s |
| **his arm** (`--env_live_water --city_env_pixel`) | **3.06 s** |

So his arm pays **+2.08 s of init on every launch**, of which 1.20 s is the
cold bake itself and 0.88 s the live-water re-shade. That is the prize for
`docs/BACKLOG_PLANS.md` item **2e** (a salted cache for the live bake, which
must also persist the mask plane). 2d/2e/2f are LOOK items and stay in his
queue — priced here, not landed.

### ROW 4 — `waterWaveSlope` 8-WIDE (2026-08-16e). BYTE-NULL, AND THE BYTE STORY IS THE FINDING

`--water_slope_vec8`, default ON. Three arms in one worktree (parent binary
`e99f5fed` / new `--no-water_slope_vec8` / new default), `--bench=scene@scene=city,
t=<T>,iters=30,xres=1512,yres=848 --profiler=1 --deferred_prof=5 --hw_prof`,
**min-of-11 over 12 interleaved rounds with r0 dropped and the arm order rotated
per round**.

| | t=1961 par → **off** → **ON** | t=2400 | t=400 |
|---|---|---|---|
| **frame min** | 47.510 → 46.890 → **46.220** (−2.7 %) | 30.730 → 30.120 → **29.590** (−3.7 %) | 35.490 → 34.340 → **33.660** (−5.2 %) |
| **TOTL** | 52.161 → 51.313 → **50.400** (−3.4 %) | 35.355 → 34.850 → **34.212** (−3.2 %) | 39.283 → 38.402 → **37.483** (−4.6 %) |
| `water-ripple` wall | 1.851 → 1.577 → **1.142** (−38.3 %) | 1.482 → 1.292 → **0.923** (−37.7 %) | 2.159 → 1.838 → **1.319** (−38.9 %) |
| `water-ripple` thrsum | 19.83 → 17.23 → **12.13** (−38.8 %) | 16.15 → 13.82 → **10.28** (−36.3 %) | 24.36 → 21.02 → **14.67** (−39.8 %) |
| `water-ripple` Ginstr/f | 0.219 → **0.243** → **0.148** (−32.4 %) | 0.179 → 0.197 → **0.126** (−29.6 %) | 0.257 → 0.285 → **0.169** (−34.2 %) |
| `water-glints` wall | 2.423 → 1.891 → **1.590** (−34.4 %) | 1.968 → 1.557 → **1.345** (−31.7 %) | 2.509 → 1.960 → **1.667** (−33.6 %) |
| `water-glints` thrsum | 26.77 → 20.68 → **17.18** (−35.8 %) | 21.26 → 16.76 → **14.61** (−31.3 %) | 28.48 → 22.18 → **19.00** (−33.3 %) |
| `water-glints` Ginstr/f | 0.229 → 0.254 → **0.203** (−11.4 %) | 0.179 → 0.202 → **0.163** (−8.9 %) | 0.239 → 0.266 → **0.212** (−11.3 %) |
| `renderFrame` Ginstr/f | 4.175 → 4.172 → 4.175 | 2.266 → 2.267 → 2.267 | 3.180 → 3.178 → 3.180 |
| `lighting-w1` / `gbuffer` / `fastfog` / `cones-call` Ginstr/f | unmoved | unmoved | unmoved |

`renderFrame` is FLAT by construction — both water passes run outside it — so
the frame-level number is the whole prize: **−1.3 / −1.1 / −1.8 ms of frame min,
−1.76 / −1.14 / −1.80 ms of TOTL**, which is the two water rows' combined wall
(4.27 → 2.73 ms at t=1961) landing on the frame.

**THE OFF COLUMN IS NOT A NULL, AND THAT IS WHY IT IS WORTH A COLUMN.** Batching
alone — the same loops, the flag off, eight scalar `waterWaveSlope` calls per
flush — *costs* instructions (`water-ripple` 0.219 → 0.243, +11 %: the batch
bookkeeping) while *winning* wall (1.851 → 1.577, −15 %): eight independent
calls back to back overlap in the out-of-order window where the pre-batch loop
interleaved each call with its own branchy tail. The vector form then takes
0.243 → 0.148 on top. Quoting only par-vs-default would have credited the
restructure's wall win to the vectorization.

Cycles fall further than instructions in the glints row — **Gcyc/f 0.082 → 0.054
(−34 %) against Ginstr −11 %** — because the scalar tap is one long serial chain
per pixel (fmul → frintm → fmls → fcvtzs → scvtf → bilinear) and the 8-wide form
is one chain for eight pixels. That is also why `water-glints` wall moves 34 %
on an 11 % instruction cut: its instruction count is dominated by the `powf`
lobe and the caustic tap, which this change does not touch.

**Ceiling reached, and it is 4x not 8x**: `pwater::waterWaveSlope` is **132
instructions for one pixel**; `waterWaveSlope8` is **293 for eight**, i.e. 36.6
per pixel. clang had ALREADY SLP-vectorized the scalar 2-wide (the two scroll
layers occupy the two lanes of `.2s` vectors), so the honest headroom over the
existing code was 4x and 3.6x of it is taken.

#### THE BYTE BATTERY — three separate contraction traps, all of them found by disassembly

Every pin is unmoved (below), but getting there needed three fixes, and none of
them was in the arithmetic the round set out to change:

1. **simde's `_mm256_fnmadd_ps` is not an fma on arm64.** Outside
   `SIMDE_X86_FMA_NATIVE` it is a plain `-(a*b) + c` loop body in
   `simde/x86/fma.h`, fused only because the build carries `-ffp-contract=fast`.
   Worse, `#pragma clang fp contract` is LEXICALLY scoped, so a `FP_CONTRACT_OFF`
   at the *call site* reaches neither simde's fma nor its `_mm256_mul_ps` /
   `_mm256_add_ps`. The first cut wrapped the whole function in `FP_CONTRACT_OFF`
   and the bilinear still compiled to `fmla.4s`: **84 % of 2.6 M taps disagreed
   with the scalar, some by 6e-4 — a whole neighbouring texel, not a rounding**,
   because a 1-ULP `fu` crosses a texel boundary. Rewritten with clang
   `ext_vector_type` operators in our own file (and `__builtin_elementwise_fma`
   for the sites the scalar fuses) the pragma binds to the arithmetic:
   **0 mismatches in 2 612 962 taps**.
2. **LICM hoisted a loop-invariant square out of `RenderGlints`' specular tail.**
   `Vy = ey - waterY` is invariant, and once the slope moved to a batch flush
   clang pre-computed `Vy*Vy` — which ROUNDS it, where the pre-batch code emitted
   `fmul Vx,Vx` + `fmadd Vy,Vy,·` + `fmadd Vz,Vz,·` and left it unrounded. Same
   algebra, swapped rounding, **143 px at 1 LSB** through `int(g*255 + 0.5)`.
   Pinned with an explicit `__builtin_fmaf` chain.
3. **The caustic tap's final lerp re-contracted the other way.** Moving the tap
   into the outlined flush turned `fma(1-dv, p, dv*q)` into `fma(dv, q, p*(1-dv))`.
   Fixed with `sampleWaterCellGlints`, a pinned copy used only by `RenderGlints`
   — the shared `sampleWaterCell` is left exactly as chase's `causticCellVaried`
   and the env-bake `CausticModulation` compiled it, the same "separate function
   so the default path stays byte-identical" rule `RenderGlintsVaried` follows.

The lesson worth carrying: on this tree a vectorization is byte-null when it is
written where the contraction pragma can *reach*, and the pins move for
optimizer decisions **around** the edit at least as often as for the edit.

**PINS — BYTE-NULL, all three arms identical at all three poses.** city his arm
t=1961 / 2400 / 400 `4cb8d2ca` / `f473fe2b` / `d3374de6` on parent, `--no-water_slope_vec8`
and default alike; city `--deferred` `bd4ffbf8` 3/3; chase t=100/400/800/1200/1600
`3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814` 2/2; fountain
`8db68ccb` 2/2; greets t=1588 `570a7b44` 2/2; `render_gate.sh` **4/4 PASS**.
Chase and greets are untouched by construction as well as by pin — chase's water
goes through `RenderGlintsVaried` / `waterWaveSlopeVaried`, a separate copy this
round does not open.

**NOTE ON THE RECORDED CITY PINS — RESOLVED 2026-08-16f: THERE WAS NO DRIFT.**
~~`4031ceec` (`--deferred`) and `925ecd43` (his arm) no longer reproduce…
pre-existing drift somewhere between the 2026-08-16b/c rounds and the tip.~~
**Wrong diagnosis, and no commit is responsible.** Both values reproduce
byte-exactly at tip `eb5e57d9`, 3/3 each, when the recipe omits `--profiler=0` —
which is how they were recorded. The `bd4ffbf8` / `4cb8d2ca` column above is the
SAME binary WITH `--profiler=0`: `Runtime/rev.cfg` ships `ProfilerEnable 1`,
`DEMO/CITY.CPP:3942` paints the FrameProfiler overlay into `VPage`, and
`RunCitySnapshot` — unlike `RunChaseSnapshot` — never silenced it, so the older
values carry **3 718 px of glyph text in a 114×221 block at x 0–113, y 15–235**
(max |Δ| 245, mean 215; zero scene pixels). That is also why chase, which already
had the silencer, was the one scene whose pins reproduced under both recipes.
The overlay is silenced in all four narrative snapshot loops as of 2026-08-16f
and the city pins are now **`bd4ffbf8`** / **`4cb8d2ca`** — reproducing with or
without the flag. Full argument, quantification and picture: the 2026-08-16f
block at the top of `docs/SESSION_STATE.md`.

#### WHAT IS STILL SCALAR

The **`--env_live_water` tilt**, the third consumer. It reaches the field through
`fds::g_envLiveWater.slopeFn`, a function POINTER called per glass pixel inside
the deferred kernel's per-lane loop, so batching it means restructuring
`Render_DeferredLighting_Tile_OuterVec`'s lane walk, not this file. It is the
0.041 Ginstr/f `--env_live_water` adds to `lighting-w1` at t=1961 (row 4 note
below), and `lighting-w1` is unmoved by this round, as the table shows.

**Amendment 2026-08-16z (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-16z) — the
remainder list's last three items are priced, and TWO OF THEM ARE BELOW BAR.**
Row 10's `atanf` and row 4's note "the `--env_live_water` tilt is the one
consumer still scalar" are both closed with numbers, not with a landing.

* **`Froxel_GlowTile`'s `atanf` (row 6's "unpriced", row 10's 0.3 %) — BELOW
  BAR.** `-DFDS_FOG_ATAN_CENSUS=1` counts **609 214 calls a frame** at t=1961
  (576 coarse glow columns × 32.2 lights each × ~33 slices) and answers both
  playbook questions NO: the argument `(2αz+β)/√disc` is all-distinct (not
  tableable) and invariant at no loop level (not hoistable). It also kills the
  reorder move without a build — **96.5 % of the atans CONTRIBUTE**, only 0.4 %
  are thrown away by a later test, and deferring the atan costs an extra one per
  contiguous run. `-DFDS_GLOW_ATAN=n` then prices it: deleting the atan
  ENTIRELY is `fog-glow` 0.092 → 0.061 Gi/f (−33.7 %) and **`renderFrame`
  −0.79 % at t=1961 / −0.47 % at t=400**, with the frame wall not resolving it
  at t=400 at all; a polynomial atan — the only attack the census leaves —
  collects **0.24 %**, under the bar, and is a numerics judge call on a
  *difference* of atans where cancellation amplifies its relative error.
  **Also: `Froxel_ColumnTile`'s pass-2 copy of the loop makes ZERO calls in
  city** (it needs a shadow-casting or flash light), so 100 % of row 10's
  `atanf` is the glow grid's.
* **The `--env_live_water` tilt's function pointer (row 4's handover) —
  REFUTED, and the note it hands over is corrected.** `-DFDS_LWTILT_CENSUS=1`:
  **94 483 / 37 429 / 103 538** `EnvLiveWater_TiltDir` calls per frame at
  t=1961 / 2400 / 400, ALL in the main pass (the reflection underlay makes
  zero). `-DFDS_LWTILT_ABLATE=n` splits the cost: a DIRECT (devirtualized,
  LTO-inlinable) call is `lighting-w1` −0.42 % / `renderFrame` **−0.10 %**,
  while zeroing the slope outright — the ceiling for ANY lane-walk restructure
  — is `lighting-w1` −1.57 % / `renderFrame` **−0.38 %** (t=400: −2.20 % /
  **−0.57 %**). So **the call is 27 % of it and the arithmetic 73 %**, and an
  8-wide form at 2026-08-16e's measured 3.6× headroom tops out at **0.26 % of
  the frame** before paying for the restructure that 2026-08-16h measured at
  +8 % to +23 % on register pressure. Not built.
  **CORRECTION to this section's "the cost is the wave-slope call, not the mask
  read":** of `--env_live_water`'s +0.041 Gi/f at t=1961 the slope evaluation
  is **0.015** and the mask read + weight + plane-hit + re-projection is
  **0.026**.
* **Row 6's `Froxel_CompositePixel` (3.4 %) — the punt is REFUTED and the
  census found the item that pays.** `-DFDS_FOG_PUNT_CENSUS=1`: 41 898 of
  152 640 groups punt at t=1961 (27.4 %) and **87.7 % of them have all EIGHT
  lanes reflective** (94.2 % of punted-group lanes are), so "punt only the
  LANES" recovers ≤6 %. What it did find: **61 056 TAIL pixels per composite
  pass** — `tsx = ceil(1512/12) = 126 = 15 groups + 6`, so
  `Froxel_CompositeTileVec8`'s tail loop hands 6 px × 106 rows × 96 tiles to
  the scalar path, 4.76 % of the frame, in both composite passes.
  **`--fog_composite_tile_align8` (default ON, BYTE-NULL at 12 pins)** rounds
  the composite's tile X span up to a multiple of 8. **It is a measured NO-OP
  at 1920** (ceil(1920/12) = 160 is already aligned) and pays at 1512, 1280,
  1024, 800, 640, 1366, 2560.

### THE RANKED TABLE UNDER THIS ARM — city t=1961

Per-symbol shares are Instruments Time Profiler self time over a 400-iteration
bench (frame-dominated: init is ~3 s of ~28 s).

| # | item | symbol share | Ginstr/f | attack |
|---|---|--:|--:|---|
| 1 | `Render_VolumetricCones_Tile` | **20.6 %** | 1.288 (`cones-call`, ×1) | rounds 6–7 took −29 % of chase's; city's is §13's dependency chain. Only "fewer (px × spot) pairs" is left |
| 2 | `Render_DeferredLighting_Tile_OuterVec` | **15.3 %** | 0.957 (`lighting-w1`, ×2) | of which `--city_env_pixel` +0.129, the env compose +0.089, `--env_live_water` +0.041 |
| 3 | `FrustumClipper::Render` | **6.2 %** → **1.2 %** | inside `gbuffer` | **DONE 2026-08-16c (`c26c1c35` + `d9dfa527`), and the mechanism in this cell was only half right.** The walk was the smaller half: `--tile_bbox_cull` was **INERT on the mirror pass**, whose hand-written `Reflected_Transform` pushed a 2-field aggregate and left the cover-all bbox default on every entry — 621 180 (face, tile) clipper entries against the main pass's 29 671 on the same geometry. Stamping it + binning faces to tiles: this symbol **6.46 → 1.21 % of self time**, and with the walk row (2.53 → 0.16 %) **8.99 → 1.47 %**; `gbuffer` **0.658 → 0.444 Ginstr/f (−32.5 %)**, thrsum 79.3 → 43.1 core-ms; `renderFrame` 4.383 → 4.174 Ginstr/f. Byte-null. See §00c |
| 4 | `pwater::waterWaveSlope` | **6.2 %** → ~5.4 % → **8-wide** | 0.451 → **0.351** (ripple + glints) | **DONE 2026-08-16e — `--water_slope_vec8`, BYTE-NULL.** 132 instructions/px → 293 for eight (36.6/px); the scalar was already SLP'd 2-wide so the real ceiling was 4x and 3.6x is taken. `water-ripple` **0.219 → 0.148 Ginstr/f (−32.4 %)**, wall 1.851 → 1.142; `water-glints` 0.229 → 0.203 (−11.4 %) but **Gcyc −34 %** (the serial chain is what went, not the `powf`), wall 2.423 → 1.590. Frame min **−2.7 / −3.7 / −5.2 %** at t=1961/2400/400 — all of it outside `renderFrame`, which is flat. Three contraction traps had to be pinned first (simde's fma is not an fma on arm64; LICM pre-rounding an invariant square; the caustic lerp re-contracting) — see the subsection above. The `--env_live_water` tilt is the one consumer still scalar: it goes through a function pointer inside the deferred kernel's lane loop |
| 5 | `meka::TileRasterizer::apply_exact<false>` | 5.4 % | 0.659 (`gbuffer`, ×2) | `effPar` 8.5–8.7 of 12 — better than chase's 5.5, so §00 row 9's "half the pool is idle" does not hold in city |
| 6 | fastfog lambdas + `Froxel_CompositePixel` + `FastFog_SampleGrid` + `SkyPaint` | 6.2 + 3.4 + 2.7 + 1.6 % | 0.869 → **0.857** (`fastfog`, ×1) | **PARTLY DONE 2026-08-16z.** The `atanf` is priced and BELOW BAR (ceiling 0.79 % of the frame, realistic attack 0.24 %; see the amendment above). `Froxel_CompositePixel`'s punt is REFUTED — 87.7 % of punted groups have all EIGHT lanes reflective — but the census found **61 056 TAIL px per composite pass** going scalar because `ceil(1512/12) = 126` is not a multiple of 8. **`--fog_composite_tile_align8`** (default ON, byte-null) removes them: `fog-composite` **0.376 → 0.365 Gi/f (−2.9 %)**, `fastfog` −1.3 %, `renderFrame` −0.26 / −0.53 / −0.41 % at t=1961/2400/400 against a 0.00 % floor — **and a measured NO-OP at 1920**, where the split is already aligned. What remains is the 335 184 punted px/frame ≈ **0.030 Gi/f**: the water-reflection branch written 8-wide |
| 7 | `vFogNoise` + `vBlobNoise` | 4.7 + 0.8 % | inside `fog-columns` | §00 row 8's "the noise is the cost" reproduces exactly here |
| 8 | `Render_DeferredTransparentLighting_Tile<0>` | 3.4 % | 0.531 (`TBR-render`, ×2) | |
| 9 | `LightMeshVerts` | 1.7 % | none (outside `renderFrame`) | DONE above — the 1.7 %/6 ms split is the whole story |
| 10 | `logf` / `powf` / `atanf` | 0.9 / 0.9 / 0.3 % | — | `logf` was the froxel composite (fixed); the rest is the glint lobe and the glow integral. **`atanf` COUNTED 2026-08-16z: 609 214 calls a frame at t=1961, ALL of them `Froxel_GlowTile`'s** (`Froxel_ColumnTile`'s pass-2 copy makes zero in city), 51 instructions each, 0.031 Gi/f. Not tableable, not hoistable, 96.5 % of them contribute. **Closed below bar** |

---

## 00a. THE USER'S ACCEPTANCE ARM — greets, 1512x848, 2026-08-16

```
./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace
```

> **AMENDED 2026-08-16y — THE DIAL LANDED. `ssao_downscale` now defaults to
> `2`, countersigned by Gil-Ad verbatim: "ssao downscale 2 is ok (no downscale
> looks much better, but too slow)."** Every wall figure in the tables below is
> a **`--ssao_downscale=1`** number and stays valid only under an explicit
> `--ssao_downscale=1`; the shipping arm is now ~9.6 ms faster than all of them.
> **REALIZED, min-of-11 interleaved parent-vs-child, one worktree, 1512×848,
> this exact arm:** frame **49.25 → 39.62 ms** at t=5743 and **41.39 → 31.86 ms**
> at t=6097 (−19.6 % / −23.0 %); `ssao` **14.55 → 4.96** and **14.23 → 4.93 ms**;
> `ssao` Ginstr/f **1.650 → 0.603**; `renderFrame` Ginstr/f **4.686 → 3.639**
> (t=5743) and **4.191 → 3.143** (t=6097). `gbuffer` and
> `DeferredLighting-call` flat to three decimals of Ginstr/f in the same batch.
> The projection in the dial table (−9.1 ms) was slightly conservative; realized
> is **−9.6 ms** at both poses. Pins, look evidence and the both-directions
> proof that this is pure default motion: `docs/SESSION_STATE.md` 2026-08-16y.
> **CONSEQUENCE FOR THE RANKING BELOW: `ssao` is no longer row 1 of this arm.**
> At t=5743 it drops from 1.650 to 0.603 Ginstr/f, i.e. from 33 % of
> `renderFrame` to ~17 %, and `DeferredLighting-call` (1.73) is row 1 again by a
> wide margin. Re-read every "the biggest lever is the dial" sentence below as
> spent.

**This is a different frame from every table in 00 and 0.** No prior greets
round carried `--ssao --ssao_gtao`; `--ssao_downscale` defaulted to **1** when
these tables were taken (it defaults to **2** since 2026-08-16y), so
GTAO ran at FULL resolution here. Poses are the SCENE'S OWN scripted camera at
t=2845 / 3409 / 5743 / 5813 / 6097 (no `FDS_GREETS_CAM`), 12 iters,
`--deferred_prof=1 --hw_prof --profiler=1`, min-of-6 over 7 interleaved rounds
with round 0 dropped. Loads ran 15–39 across the session, so **read `Ginstr/f`
whenever a wall figure looks surprising** — `ssao`'s is stable to 0.2 % across
poses and batches, the wall is not.

### BEFORE — the map as his command found it (parent `f25bb992`)

| # | phase | ms t=5743 | Ginstr/f | share of `renderFrame` |
|---|---|--:|--:|--:|
| 1 | **`ssao`** | **19.4** | **2.143** | **39 %** |
| 2 | `DeferredLighting-call` | 17.6 | 2.040 | 35 % |
| 3 | `gbuffer` | 8.5 | 0.986 | 17 % |
| 4 | `shadow-bake` (outside `renderFrame`) | 2.5 | 0.207 | — |
| 5 | `RTT` | 2.1 | 0.024 | — |
| 6 | `bloom-chain` | 1.19 | 0.136 | 2 % |
| 7 | `cones` | 0.73 | 0.063 | 1 % |
| 8 | `tonemap-post` | 0.45 | 0.037 | 1 % |
| 9 | `mirror-grid` | 0.45 | — | 1 % |

Per-flag ablation at t=5743, one batch: `--no-ssao` frame **58.26 -> 39.33**;
hemisphere instead of GTAO `ssao` 19.4 -> 12.8; **`--texture_filter=2` costs
1.4 ms**, all of it `gbuffer` (8.50 -> 7.07, 0.986 -> 0.813 Ginstr) — the same
verdict 0 reached for `=1`, re-measured for `=2` on this arm;
`--no-bloom` 1.19 ms; `--greets_displace` itself 5.1 ms of frame (his look).

### AFTER — `b0905ee1`, the four SSAO rungs (docs/OPTIMIZATION_BACKLOG.md 2026-08-16)

Interleaved parent-vs-tip, min-of-8, both binaries one worktree one asset tree:

| pose | frame min | `renderFrame` | `ssao` ms | `ssao` Ginstr/f | `ssao` Gcyc/f |
|---|--:|--:|--:|--:|--:|
| t=2845 | 59.53 -> 59.05 | 49.67 -> 48.92 | 19.55 -> 15.06 | 2.150 -> 1.654 | 0.649 -> 0.488 |
| t=3409 | 68.45 -> 65.54 | 59.47 -> 52.94 | 22.27 -> 16.28 | 2.141 -> 1.653 | 0.637 -> 0.487 |
| t=5743 | 77.02 -> 71.29 | 66.45 -> 59.16 | 23.55 -> 16.36 | 2.143 -> 1.652 | 0.651 -> 0.492 |
| t=5813 | 58.30 -> 52.54 | 50.68 -> 45.20 | 20.18 -> 14.26 | 2.149 -> 1.650 | 0.655 -> 0.483 |
| t=6097 | 50.17 -> 44.41 | 45.10 -> 39.23 | 19.81 -> 14.14 | 2.155 -> 1.653 | 0.647 -> 0.487 |

`renderFrame` **Ginstr/f -9.0 to -10.2 %, Gcyc/f -9.3 to -10.5 %** at every
pose. Image cost of all four rungs: 343 px of 24.9 M at 1/255.

### The ranking AFTER, by instructions — the lighting kernel is #1 again

**Amendment 2026-08-16c (§00c) — and it is a NEUTRAL result, say so.** greets
gets only the binning half of that round: it has no `Reflected_Transform`, so
the 21× mirror-pass finding that moved city does not reach it. On this arm
`--face_tile_bin` is **`gbuffer` 1.001 → 0.991 Ginstr/f (−1.0 %), thrsum
96.7 → 94.8 core-ms, wall 8.81 → 8.69**, and `renderFrame` moves **−0.1 %
instructions / −1.6 % cycles at t=5743 and 0.0 % / +0.8 % at t=2845** — i.e.
inside the noise at frame level. It is kept because it is byte-null, because
the city arm does gain from it, and because it is what unblocks the finer-grid
experiment; it is NOT a greets win and nothing here should be quoted as one.

**Amendment 2026-08-16g (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-16g) — row 1 is
now itemised end to end, and it MOVED.** `DeferredLighting-call` splits
2.040 = `lighting-w1` 1.720 + `lighting-w2` 0.306 + setup 0.013; w1 splits into
the omni loop 1.247 and a per-pixel FLOOR of 0.473 that no prior round had an
instrument for (`-DFDS_PIX_ABLATE=n`, committed). Inside the loop the cube tap
is **0.747 Gi/f = 36.6 % of the whole call**; inside the floor the two biggest
items are view-pos + SH ambient (0.104) and the normal decode + TBN (0.100).
**`lighting-w2` — the checkerboard fill — is 15 % of the call and had never
been profiled.** Two flag facts the older text gets wrong on this arm: `--pbr`
and `--shadow_dynamic` are BOTH setDefault-ON in `GREETS.CPP`, so the specular
lobe is GGX (not `pow_glossClass`) and `lmKernelEnabled` is FALSE (the static
shadow lightmap is bypassed; every static omni takes a full cube tap).
`--deferred_cube_direct` + `--deferred_fill_hdr_skip` + `--deferred_lm_addr_skip`
then took the call to **1.877 Gi/f (−7.9 %)** and `renderFrame` to
**4.827 (−3.2 %)**, byte-null at nine pins and at five poses under this exact
arm. Full table, the two refutations (the GGX hoist clang already does; the
4×4 uniformity pyramid) and the ranked remainder are in the backlog entry.

**Amendment 2026-08-16h (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-16h) — row 1's
`lighting-w2` line is now itemised, and 16g's guess about it is REFUTED.** The
fill splits (t=5743, `-DFDS_W2_ABLATE`): skeleton + early tests 0.026, centre
oct normal decode 0.026, neighbour index setup 0.008, centre `fetchTexel` 0.019,
**the two-neighbour compat+accumulate loop 0.159 (54 %)**, write-out 0.056.
`-DFDS_W2_CENSUS=ON` counts 641 088 cells at 1512×848 of which **99.14 % are
AVERAGED and 768 — 0.12 % — take the full-shade fallback**: 16g's open question
"how much of w2 is the edge fallback" answers *essentially none*, and the edge
classification is not a cost target. 1.980 of 2 neighbours are compatible, so
nearly every per-pair instruction is paid twice per cell, and ONE oct decode
costs 0.026 Gi/f. Landed: the neighbour material hoist (16g's item 1, **no
flag** — a flag in this loop costs what it saves) and **`--deferred_fill_ldr_skip`**,
which drops the fill's 8-bit VPage average because the tonemap overwrites every
VPage pixel unconditionally. Gated on a new `ctx.ldrDiscarded` promise, NOT on
`hdrWrite` — `CITY.CPP:3823`'s water-reflection underlay has a live `hdrBuf`,
never tonemaps, and reads its own VPage back. Together **`lighting-w2`
0.293 → 0.272 (−7.2 %), `DeferredLighting-call` 1.876 → 1.855 (−1.1 %),
`renderFrame` 4.826 → 4.804 (−0.5 %)**, −7.0 to −7.4 % at all five poses,
byte-null at nine pins, `render_gate.sh` 4/4 and the five-pose arm diff. A
scanline carry for the repeated neighbour gather was tried in three forms and
**nets zero every time** — the backlog entry has the OFF columns. The same LDR
argument applied to WAVE 1 (1.570 Gi/f) is untried and is the largest thing the
round leaves behind.

**Amendment 2026-08-16i (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-16i) — the
sentence directly above is TRUE ABOUT THE ARGUMENT AND MISLEADING ABOUT THE
SIZE.** The LDR argument does reach wave 1, in full. The chain it reaches is
**0.033 Gi/f**, not 1.570: `-DFDS_W1LDR_ABLATE=1` (committed; stage 0 builds a
binary `cmp`-identical to its parent) removes `fdB/fdG/fdR` → `outB/outG/outR` →
`out[i]` and everything hanging off them, and reads **1.570 → 1.537 at t=5743
(−2.10 %)** and **1.457 → 1.425 at t=2845 (−2.20 %)** — 2.1 % of `lighting-w1`,
1.8 % of the call. That agrees with 16g's own split, which priced "compose
(after the loop)" at 0.060: the LDR half is 0.033, the HDR store is the rest.
**`lighting-w1` is still 84 % of the call and the cube tap is still 36.6 % of
it — nothing in this round moves that ranking.** Read the 1.570 above as "the
wave the argument lands in", never as "the size of the prize".

Landed: **`--deferred_shade_ldr_skip`** (default ON), which needs **one term
more than the fill's** — `ctx.ldrDiscarded` **AND** `--hdr_linear`, because
under plain `--hdr` the shipped B1 radiance *is* `hB = fdB + sB` and the chain
is LIVE; only the linear arm builds `rlB` from `dlB`, the raw light accumulator.
`lighting-w1` **1.570 → 1.542 (−1.78 %)**, `DeferredLighting-call`
**1.855 → 1.827 (−1.51 %)**, `renderFrame` **4.804 → 4.777 (−0.56 %)**, −1.72 to
−1.96 % of w1 at all five poses, `lighting-w2` flat to four decimals (the
control that says this touched wave 1 alone). Its OFF arm costs +0.64 to
+0.83 %, so the mechanism is −2.5 % against itself. Byte-null: nine pins at
their recorded values, five poses under his arm 3/3, the city underlay control
`56f6aff0` on both binaries. **Read the gate claim carefully:**
`render_gate.sh` is 4/4 but **none of its four arms puts `--hdr` on a main
deferred pass**, so it cannot discriminate this flag at all — the greets t=1588
pin (greets `setDefault`s `hdr_linear`, so the flag IS live there) and the
five-pose arm are the only real coverage. `GreetsMirror.cpp`'s "wave-2 fill
reaches g_hdrBuf only via the VPage lift" comment was adjudicated **STALE** (the
fill has stamped `h[3]=1` on all three arms since 2026-07-02) and rewritten; the
override-bake exclusion inside `ldrDiscarded` is over-broad by exactly two
passes, both priced at ~zero here, both written up with their exact predicates
in the backlog. Also recorded there: the **fountain t=2500 pin flipped once in
43 PARENT runs** (24/24 on a clean re-gate, both binaries) — pre-existing, ~2 %,
and a battery that reads that as a regression will burn a session.

**Amendment 2026-08-16l (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-16l) — the
cube tap's interior is now measured, and the row is REDUCED, not closed.**
16g/16i left "the cube tap is 36.6 % of the call" as the last big item with the
interior unmeasured. `-DFDS_CUBE_ABLATE=n` (committed) splits it: at t=5743 the
tap is **0.599 Gi/f over 2.943 M calls = 204 instructions**, of which **125
(61 %) are a projection prologue** — face select, the `viewToLight` 3x3, the
near/frustum/bounds rejects, 1/lz and two muls — and `otool -tvV` counts exactly
125 instructions between the same two points, so ladder and disassembly agree.
`--shadow_tap_census` adds that **81.1 % of taps never read a texel**: the
branchy, memory-bound half is the rare half.

**`--deferred_cube_prepass` (default ON) moves that prologue 8-wide over
PIXELS for a fixed light**, once per tile row, and leaves the tail scalar and
lazy behind the omni loop's own early-outs. It cannot go wide the other way:
pixel-major means eight lights carry eight matrices, so the matrix would be
GATHERED at exactly the cost of the scalar's loads; light-major makes it a
BROADCAST. `lighting-w1` **1.542 → 1.479 (−4.1 %) at t=5743**, `Gcyc/f`
0.442 → 0.381 (−13.8 %), core-ms 155.1 → 133.4 (−14.0 %),
`DeferredLighting-call` 1.827 → 1.764 (−3.5 %), `renderFrame` 4.777 → 4.714
(−1.3 %) and its wall 43.34 → 41.54. `lighting-w2` unmoved to four decimals at
all five poses.

**READ THE COLUMNS BEFORE QUOTING THIS ONE.** At **t=3409** the mechanism
retires **+1.6 % MORE instructions** while taking **−4.2 % cycles and −5.5 %
core time**, and the frame there is a WASH on wall. Cycles, core-ms and wall
agree at all five poses; instructions agree at four. Confirmed on a quiet
machine over three interleaved rounds, reproducing to ±0.001 Gi. IPC goes
3.44 → 3.64 (t=3409) and 3.46 → 3.84 (t=5743): the change trades retired
instructions for issue rate, converting twelve dependent loads and an 18-flop
matmul per (pixel × light) into wide independent work. **A future round that
watches `Ginstr/f` alone will read t=3409 as a regression — it is not.**

BIT-EXACT, and by a counter: `--deferred_cube_prepass_verify` re-runs the whole
scalar tap behind every cached one, with the pixel body's own view and world
positions, and compares bit patterns — **0 mismatches in 47 M taps**. Five
acceptance poses and nine 2026-08-16f pins identical parent-to-child.
`render_gate.sh` is 4/4 and **cannot discriminate the flag**: instrumented, its
four arms and city / chase / fountain take **zero** taps through the prepass,
because the gate needs `lmKernelEnabled == false` (`--shadow_dynamic` without
`--shadow_lm_dynamic`) plus PolyId, and greets is the only scene that sets it.
The hatch is a TEMPLATE parameter, not a bool: as a runtime test it cost
+4.3 % of `lighting-w1` on the OFF arm — the register allocator, not the
branch — and four shapes were tried before templating fixed it (OFF now +0.3 %).
**The fountain t=2500 flip that 16i recorded appeared again**, on the PARENT
binary, in this round's first three runs; 6/6 clean on re-gate, both binaries.

**Amendment 2026-08-16m (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-16m) — 16l's
"cheapest remaining item" is REFUTED, and its premise had gone stale under its
own win.** 16l parked "`CubeShadow_Sample` still spills nine callee-save pairs
into a 144-byte frame; split the tail's RARE half into a `noinline` function to
make the common path a leaf". Three corrections, all measured on the tip:

1. **It is eight pairs, not nine** (`otool -tvV`).
2. **That function is 0.43 % of taps.** A per-entry counter
   (`-DFDS_TAPPATH_CENSUS`) at greets t=5743 on his arm: 2 943 120 taps/frame
   go through `CubeShadow_SampleCached` — *inlined into the tile kernel* by
   16l's own `--deferred_cube_prepass` — against **12 593** through the
   out-of-line `CubeShadow_Sample` that owns the frame. 19.53 % of all taps
   reach the 2x2 PCF.
3. **The frame was never the PCF's register pressure.** `CubeShadow_Sample`
   contains exactly ONE `bl`, and it is `ShadowSwzGetShape()` — a function-local
   `static` whose getenv/sscanf/fprintf initialiser forces a thread-safe guard
   and a cold `bl __cxa_guard_acquire`. That one never-taken branch
   (`--shadow_swizzle` is default OFF) is what pins sixteen registers callee-save
   across a body whose five early rejects all run before it.

**Publishing the shape to a plain global** (30 lines, `g_shadowSwzShape`) makes
`CubeShadow_Sample` a **leaf: 8 callee-save pairs → 0, 144-byte frame → none,
410 → 387 instructions, and no call added anywhere.** It fixes the 2-D spot tap
`computeMapShadowAtten` in passing (2 `bl` → 1, 10 → 9 pairs, 0xc0 → 0xa0
frame). `lighting-w1` **Gcyc/f −1.17 to −2.85 % at ALL FIVE poses**, Gi/f −0.96
to 0.00 % (win at four, flat at the fifth), `renderFrame` wall −0.90 to −1.50 %
at four poses and **+0.72 % at t=6097** — where both counter columns move
hardest the other way (`renderFrame` Gcyc −2.40 %), i.e. the mirror image of
16l's t=3409, so **quote the pose, not the summary**. BIT-EXACT: 0 mismatches in
76.8 M taps under `--deferred_cube_prepass_verify`, five poses and the nine 16f
pins at their recorded values, `render_gate.sh` 4/4 (and, exactly as 16l says,
**it cannot discriminate a greets-only tap change**). Shipped FLAGLESS.

**Amendment 2026-08-16n (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-16n) — 16m's
named remainder, the 2-D SPOT tap: NO LEAF IS AVAILABLE, and it did not need
one.** 16m parked "`computeMapShadowAtten` is still not a leaf: 505
instructions, one `bl` left, nine pairs, 160-byte frame — same method". The
`bl` is `CubeShadow_Sample` on the mirror-clone `srcCube` arm: a **real hot
callee**, not a lazy-static guard, so publish-to-global does not transplant.

1. **PRICED FIRST, and it is a 400x range across poses.** A per-call census
   (`-DFDS_SPOTCALL_CENSUS=1`, counted inside the function so it sees past the
   scalar site's 3-plane guard), per main-view frame at 1512x848 on his arm:
   **t=6097 0 calls · t=5813 3 984 · t=5743 14 742 · t=2845 49 368 · t=3409
   828 452 · t=3122 1 771 205** (t=1588 pin: 151 411). The `srcCube` arm takes
   99.71 % of them at t=3409 and 93.35 % at t=3122. **The VEC call site makes
   ZERO calls at every pose, and city (both arms), chase (all five pin poses)
   and fountain make zero calls of any kind** — the spot tap's home is greets
   alone, like the cube tap's. So the 22-instruction frame is 0.00008–0.001
   Gi/f at three of the five acceptance poses (**below the 0.01 Gi/f bar,
   closed there**) and 0.018 / 0.039 Gi/f at t=3409 / t=3122.
2. **The leaf is genuinely unavailable.** Force-inlining the CALLEE (`inl`)
   deletes the `bl` and still keeps **8 callee-save pairs and a 128-byte
   frame** — the state live across the cube tap is real, unlike 16m's
   never-taken branch. Outlining the `srcCube` arm is refused by its own
   census: it buys a leaf for 0.29 % of calls and adds a call to the other
   99.7 %, ~+1.60 % predicted at t=3409 — 16l's `spl` arithmetic with a new
   denominator.
3. **What pays is deleting the CALL from the caller's side**
   (`[[clang::always_inline]]` on the scalar call site — that site is 100 % of
   the calls). `lighting-w1` **Gi/f −2.00 % at t=3409 and −4.27 % at t=3122**,
   **Gcyc/f −2.61 % / −5.01 %**, `renderFrame` Gcyc −1.14 % / −3.66 %, wall
   −0.68 % / −0.54 %; and **one least-significant digit (+0.001 Gi/f) at the
   three poses where the tap barely runs**, with the Gcyc moves there (+0.83 %
   at t=2845, +0.27 % at t=5813) inside the measured 1.4–4.6 % round-to-round
   spread of that column. Min over 11 order-rotated interleaved rounds.
4. **HALF THE WIN IS NOT THE FRAME.** The ABI accounting is 11 (prologue) + 11
   (epilogue) + 1 (`bl`) + 12 (the callee's argument-shuffle `mov`s) + ~11
   (caller marshalling) = **~46 instructions per call**, because the tap takes
   **17 arguments, ten of them floats — two of which travel by STACK**.
   Predicted −2.45 % / −4.58 % against measured −2.00 % / −4.27 % at call
   counts **2.14x apart**; the frame alone would have predicted −1.17 % /
   −2.19 %. **On this tap the argument list costs as much as the frame.**
5. **THE TAX, measured where it cannot pay**: greets t=6097 (zero calls)
   +0.08 % Gi/f — one LSB; **city t=1961 on his acceptance arm, 11 rounds:
   `lighting-w1` Gi/f +0.00 %, `renderFrame` Gi/f +0.02 %**. The +530
   kernel instructions cost nothing measurable outside greets.

BIT-EXACT: 14 surfaces (five poses + the nine 16f pins) identical on the FIRST
pass for all three built arms, pins at their RECORDED values; the committed
`--shadow_tap_census` **spot-pyramid rows identical parent-to-child at all five
poses**; `--deferred_cube_prepass_verify` 0 mismatches in 76.8 M taps (which
does **not** cover the `srcCube` arm — the t=3409 frame, 99.71 % `srcCube` and
byte-identical, is that arm's coverage); `render_gate.sh` 4/4 and **it cannot
discriminate this change at all** — its four arms make zero
`computeMapShadowAtten` calls. **The fountain t=2500 flip did NOT appear this
round** (4 clean passes). Shipped FLAGLESS: `always_inline` has no runtime
dial, and a template hatch would duplicate a 4827-instruction kernel to gate a
one-line placement change. **THE CALL FRAME IS NOW CLOSED ON BOTH SHADOW
TAPS**; what remains on the spot tap is the `srcCube` arm's mirror-reflection
ARITHMETIC (~40 float ops x 0.83–1.77 M calls/frame), which is a different
round.

**The literal split was built and it LOSES at every pose** — `lighting-w1`
+0.51 to +1.54 % instructions AND +0.23 to +1.42 % cycles — because it buys the
leaf by adding a real call on the 19.6 % of taps that reach the PCF:
577 296 calls × ~34 instructions = 0.0196 Gi/f = +1.33 % predicted against
+1.42 % measured. A hatched version is worse still (+1.36 to +1.82 %) and the
disassembly says why before the clock does: a flag has to keep the inlined body
in the kernel for its OFF arm to switch to, so its ON arm cannot collect the
win. **The call frame is now CLOSED as a lever on the cube tap.**

**Amendment 2026-08-16o (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-16o) — 16h's
item 1 is DONE, and its own named form was a quarter of the prize.** 16h left
"the three oct normal decodes, ~0.078 Gi/f = 29 % of the fill" with a
`oct_decode_u32_x2` over the TWO NEIGHBOURS as the bit-exact candidate. Both
were built. The 2-wide is **−2.6 to −3.6 % of `lighting-w2`**; a **4-wide that
also carries the CENTRE on lane 2** (`meka::oct_decode_u32_x4`, one decode where
the fill did three) is **−11.9 to −13.4 % at six poses**: `lighting-w2`
**0.272 → 0.239** at t=5743, `DeferredLighting-call` **1.762 → 1.729 (−1.87 %)**,
`renderFrame` **4.712 → 4.680 (−0.68 %)**, −1.87 to −2.28 % of the call and
−0.58 to −0.85 % of the frame at t=2845 / 3409 / 5743 / 5813 / 6097 / 3122.
`lighting-w1` **flat to four decimals at all six** — the control that says only
wave 2 moved.

**Why the widths differ is a LIVE RANGE, not a lane count** (a `.2s` and a `.4s`
op cost the same on this core). Per cell, from the disassembly: parent 110
instructions, 2-wide child 82, 4-wide child 54. The 4-wide delivers its
arithmetic in full — 51 predicted against 51.9 measured (0.0330 Gi/f over
636 349 cells), ladder and `otool` agreeing to 2 %. The 2-wide delivers **55 %**
of its 23/cell, because it leaves `ncX/ncY/ncZ` decoded at the top of the cell
body and live across the whole neighbour loop; the 4-wide consumes the centre
inside the block (by-element, `fmul.2s v0, v0, v0[2]`) and the three scalars die
at once. **16h's "this one removes registers rather than adding them" was right
about the mechanism and wrong about which version has it.**

BIT-EXACT, and read from the disassembly before it was claimed: every operation
the scalar `oct_decode_u32` compiles to is element-wise on AArch64, the two
fused `fmadd`s of the length and the compiler's chosen dot order
(`fma(ncZ,nz, fma(ncX,nx, ncY*ny))`) are reproduced term for term with
intrinsics, and `fast_rsqrt` was already `vrsqrte` + one Newton step on a
`float32x2_t`. The one shape change is the `az < 0` fold — a BRANCH in the
scalar, a SELECT in the vector because the lanes disagree — and both arms are
finite for every input word. Checked, not just argued:
`-DFDS_W2_OCTPAIR_VERIFY=ON` (committed) runs the scalar behind every lane and
compares BIT PATTERNS — **0 mismatches in 57.3 M lanes** on normals, dot and
verdict. The same counter prices the fold at **62.8–95.2 % of lanes**, i.e. the
branch the vector gives up was mostly being taken anyway.

**READ THE FLOORS.** The parent's own round-to-round spread over 11 kept rounds:
`lighting-w2` Gi/f **0.00–0.37 %**, Gcyc/f 3.5–5.6 %, wall 3.4–16.6 %;
`renderFrame` Gi/f 0.02–0.12 %, Gcyc/f 0.9–1.9 %, wall 3.1–5.6 %. The
**instruction column is what resolves this change at every level**; the w2 cycle
win (−7 to −13 %) is 2–3x its floor and real; **`Gcyc/f` and `wall` at
`renderFrame` are inside their floors and prove nothing here** — the OFF arm,
which can only add work, reads −3.6 % and −6.5 % frame cycles at two poses in
this batch. Do not quote a frame-level time number from this round.

Shipped **FLAGLESS** — and for a different reason than 16h's hoist. Here the
hatch is **free**: `--deferred_fill_oct_pair` ON measures identical to the
flagless build to four decimals at all six poses, because the predicate is
loop-invariant and hoists out of the pixel body, where 16h's sat inside the
k-loop. So the flag buys no speed, the change is bit-exact so it needs no look
dial, and the only thing the dial can do is cost +0.7 to +1.1 % of the fill. The
three arms survive as CMake switches (`-DFDS_W2_OCTPAIR_MODE=0|2|4`,
`-DFDS_W2_OCTPAIR_HATCH=ON`). BYTES: five poses at BOTH 1920x1080 and the
1512x848 measurement resolution, t=3122, the nine 16f pins at their recorded
values 3/3, the city `--deferred_checkerboard` control `7eb0f8c4`, and every
`--omni_census` wave-2 census row identical parent-to-child. `render_gate.sh`
4/4 and **instrumented rather than trusted: all four of its arms take ZERO cells
through the fill** (no `[W2-CENSUS]` line on a census build) — the greets t=1588
pin, which takes 1 036 800 cells, plus the six-pose arm diff are the coverage.
The fountain t=2500 flip appeared once in five PARENT runs and never in five
child runs. **`lighting-w2` is now 0.239 = 13.8 % of the call**; what is left in
it is 16h's item 2 (the 3-channel scalar arithmetic, ~0.03 Gi/f) and that one is
**NOT** bit-exact by construction — clang already SLP-vectorises parts of it.

t=5743: `DeferredLighting-call` 2.041 (41 % of `renderFrame`'s 4.999),
**`ssao` 1.651 (33 %)**, `gbuffer` 1.002 (20 %), `bloom-chain` 0.136,
`cones` 0.084, `tonemap-post` 0.037. So 00's rows 1 and 5 (the omni loop and
the cube tap) are the next target on this arm too — ~~and the single largest
lever of all is a dial, `--ssao_downscale=2`, worth another -9.1 ms of frame
for a look change nobody has approved yet~~ **— SPENT 2026-08-16y: the dial was
countersigned and is now the DEFAULT, realized −9.6 ms** (`ssao` 1.650 → 0.603
Ginstr/f). With it spent, `DeferredLighting-call` at 1.73 is the unambiguous
row 1 of this arm and there is no second dial of that size left; numbers, crops
and the both-directions default-motion proof in
docs/OPTIMIZATION_BACKLOG.md 2026-08-16 / 2026-08-16y and
docs/SESSION_STATE.md 2026-08-16y.

---

## 00. ROUND-1 REBASELINE — all five scenes, 2026-08-14

Ordered by what a frame-time-reduction campaign should attack next. Everything
below was measured on `a9ca03dc` in a throwaway worktree, `SDL_VIDEODRIVER=dummy`,
1920×1080 unless stated, 12 pool workers, **min-of-6 over 7 interleaved rounds
with round 0 discarded**, arm order rotating every round. Load average is quoted
per table and ran **2.9–24.6** across the session — so read `Ginstr/f` (which
reproduces to <0.5 % across loads) whenever a wall figure looks surprising.

Instruments: `--deferred_prof=1 --hw_prof` for the phase/counter tables,
Instruments Time Profiler for the per-symbol tables, `--prof_*` gates for the
kernel interior. Drivers are committed: `scratchpad/prof1.py` (interleaved
multi-arm runner + report; arms are JSON argv **lists**, so the zsh
word-splitting scar cannot recur), `scratchpad/prof1_arms.py` (the arm matrix),
`scratchpad/prof1_symbols.py` (xctrace record + the id/ref-resolving export
parser).

### THE RANKED TABLE

`ms` is that item's own `wall_min` at the named pose. "Bound by" is measured, not
argued: IPC against the box's ladder (2.42 compute / 0.96 L1 / 0.14 L2), `effPar`
against 12 workers, and per-symbol self time.

| # | item | scene / pose / arm | ms | Ginstr/f | bound by | achievable (INFERRED) | attack |
|---|---|---|--:|--:|---|---|---|
| 1 | **deferred omni loop** (per-light shading + shadow taps) inside `lighting-w1` | greets t=5743 flat | **19.9** of 47.8 `renderFrame` | 2.490 | compute; IPC 3.51, `effPar` 11.4/12 — no idle, no stall | 4–6 ms | **PARTLY DONE — 2026-08-15, docs/OPTIMIZATION_BACKLOG.md 2026-08-15.** Itemized by a committed staged-continue ladder (`-DFDS_OMNI_ABLATE=n`, two sessions agreeing to 0.10 %), which independently reproduces this cell's 2.490 as 2.486. **The loop is a SHADOW loop: 73.5 % of it is the shadow chain at this pose (67.7 % at his), and the largest non-shadow item is the specular lobe at 10.6 %** — the attenuation / N·L / cone / portal / accumulate that this row hoped to find are 12 % between them. Shipped: `computeMapShadowAtten` is skipped when its three index planes are all absent, which `--omni_census` says is **99.50 % of its 4.749 M calls a frame** — bit-exact, no flag, **−1.75 ms `lighting-w1` / −1.78 ms frame here, −22.5 % of chase's `lighting-w1`**, city + fountain neutral. What is left is the cube tap (row 5) and it is closed to cheap edits — see the refutation there |
| 2 | **volumetric cones** | chase t=800 | ~~21.6~~ → ~~17.6~~ → **16.0** of 46.9 (×2 passes) | ~~2.817~~ → ~~2.192~~ → **1.992** | compute; IPC 4.29; was **46.6 % of chase's entire CPU self-time** | 2–4 ms | **PARTLY DONE — round 6 (2026-08-14), docs/HW_PROFILING.md §14.** The guess in this cell was right: chase runs 32 NARROW cones of 34 spots, so 91 % of its pairs took the *segmented* branch, and `--vol_cone_solve_vec` was gated `!segPath` on a round-1 GREETS measurement. Un-gating it + a new `FDS_CONE_QUADEARLYOUT` (round 1's early-out moved from the range sphere, which rejects 0.0 % of chase's pairs, to just after the cone quadratic, which rejects 83.9 %) took the cone pass **−18.4 % wall / −21.6 % instructions / −19.6 % cycles** at t=800, −19.6 %/−21.2 % at t=400/t=100, −9.4 % at t=1200, **+4.1 % at t=1600** (no segmented pairs there, 3.4 % fire rate, 3.1 ms pass). greets IMPROVED (−9.7 % at its pin pose), city cycles −0.6 %. Judge call on bytes: chase t100/t400/t800 and greets pins moved by 20–381 px at max |Δ| 2/255. **ROUND 7 (2026-08-15), docs/HW_PROFILING.md §15** then cashed the parked prologue: the spot loop is the INNERMOST of three, so 12 SoA loads, two dot products and `1/(cosI−cosO)` ran per (batch × spot) for values that depend only on the LIGHT. A per-tile `ConeSpotPre` record collapses 10 800 evaluations per tile to one — **cones −9.1 % instructions / −7.6 % cycles / −7.9 % wall at chase t=800, −10.7 % at t=400, and −8.1 % instructions on CITY too** (the first cone change that helps all three cone scenes: the prologue ran for every pair regardless of branch). **BIT-EXACT — all eight pins unmoved, render_gate 4/4 byte-identical, no judge call.** **WHAT IS LEFT**: the 8-segment body is ~31 % of the pass; its `W²`/`D·W` closed form was built and REFUTED (+0.1..+0.7 % instructions — that loop runs only on ALIVE pairs, 8.1 % of chase's — see §15.5); the per-spot shadow-map block behind `spotAlive` is unpriced; greets' `shadowed=51` per-segment tap is unpriced. |
| 3 | ~~**two-layer transparent lighting** (`TBR-render`)~~ **CLOSED 2026-08-14** | fountain t=1200 | ~~**21.7** of 28.1 (77 %)~~ → **8.17 of 13.27** | ~~3.032~~ → **1.081** | — | **−11.99 ms taken (−43.7 % of the frame)** | `<0>`/`<1>` are front-/back-FACING, not depth layers; the depth peel is `XparPeelPasses=4` on top. Cause: the per-strip dispatch bounded Y and **only** Y, so all 12 884 composite invocations a frame scanned the full 1920 px strip width — **197.90 M px scanned for 0.97 M live fragments (0.491 %)** — and peel passes 1–3 were 100 % empty. Fixed byte-null by `--xpar_strip_extent` + `--xpar_peel_early_out` (both default ON). See `docs/OPTIMIZATION_BACKLOG.md` 2026-08-14b |
| 4 | **volumetric cones** | city t=1961 | **16.6** of 62.7 (×2 passes) | 2.257 | dependency chain, not op count (HW_PROFILING §13) | 1.5–3 ms | closed as a spelling problem. Only "do less work" is left: fewer (px × spot) pairs. Culling is capped at ~3 %, unroll-and-jam at ~8 % |
| 5 | **cube shadow sampling** inside the omni loop | greets t=5743 flat | **9.1** (6.8 of it the cube tap, 2.1 the 3 extra PCF taps) | 1.159 | compute; per-symbol `CubeShadow_Sample` 16.1 % + `resolveCubeAtten` 6.1 % + `computeMapShadowAtten` 4.0 % = **26 %** of DEMO self-time | 2–3 ms | **RE-MEASURED 2026-08-15: the cube tap alone is 1.422 Ginstr/f = 43.8 % of `lighting-w1`, not the 24.5 % `--prof_no_cube_tap` reported** — that diagnostic returns *fully lit*, so the 54.6 % of taps that come back shadowed stop taking their `continue` and pay diffuse+specular in the no-tap arm (2.59 M extra shaded pairs a frame, 0.385 Gi of the gap). The interior is now REFUTED as a target: adding one bool to skip the provably-all-zero dynamic plane — four loads and eight compares per tap, algebraically byte-null — measured **+12.4 % of `lighting-w1`'s instructions**, the third independent instance of the tap being at its register-allocation limit. **Fewer taps is the only lever**; the census gives the parked 8×8 PolyId-uniformity pyramid its denominator (76.1 % of taps sit in an 8×8-uniform block) |
| 6 | **non-light kernel remainder** (G-buffer decode, matID→`Material*`, normal/metal/rough/AO/horizon fetches, ambient+SH, env compose, store) | greets t=5743 flat | **9.9** | 0.801 | compute; IPC ~2.6 in this slice | 1–2 ms | this is the one interior slice that **grew** since 2026-08-08 (8.28 → 9.88 ms). Not root-caused. Bisect it before optimising it |
| 7 | **water simulation + glints** | chase t=800 / city t=1961 | ~~chase 16.9 % of CPU~~ → chase **14.195 → 10.021**; city glints **7.720 → 4.602**, city ripple **4.015 → 3.117** | chase 1.128, city 0.434 + 0.406 | **was PARALLELISM** — contiguous row bands over a screen whose top half is a 10-instruction reject; now compute, `effPar` 11.1/12 | — | **DONE 2026-08-15d, docs/SESSION_STATE.md.** The passes had no phase row at all (they run outside `renderFrame`); `--water_census` + `runRowBands`' `effPar` column are the instruments that land with the fix. **−4.174 ms chase / −4.016 ms city / −3.550 ms of city's FRAME, BIT-EXACT** (eight pins 3/3, render_gate 4/4): 8-row dynamic chunks off an atomic cursor, plus a provable `ndhMin` skip ahead of `powf`. **This cell's premise was wrong twice**: greets has NO water, and the full-screen scan is NOT the fountain 198M-px pattern — the reject path is ~10 instructions against ~1050 per LIVE pixel, so all three bit-exact scan-side levers measured flat (0.7–1.4 %). **2026-08-15e** then took the caustic sampler (three bilinear channel taps collapsed to one number by every caller) for a further −16.9 % chase / −11.4 % city — a judge call at **7 px of 12.4 M, all |Δ|=1/255**, moving four pin values. **End to end: chase 17.559 → 9.175 (−47.7 %), city glints 8.035 → 4.574 (−43.1 %), city ripple 4.561 → 3.557 (−22.0 %).** What is left is **five libm calls per live pixel** in the chase path, priced at −20.1 % instructions for the four swell transcendentals |
| 8 | **fastfog** | city t=1961 | **10.0** | 1.105 | compute; IPC 3.51 | 1–2 ms | **never per-symbol profiled before this round.** It resolves: `FastFog_SampleGrid` 4.3 %, `Froxel_CompositePixel` 4.1 %, `vFogNoise` 3.9 %, the three `Render_DeferredFastFog` tile lambdas 5.9 %, `SkyPaint` 1.8 %, `vBlobNoise` 0.7 %. **The noise is the cost** — `vFogNoise`+`vBlobNoise` ≈ 4.6 % of city's CPU |
| 9 | **G-buffer fill parallelism** | chase t=800 | 11.3 ms at `effPar` **5.5 of 12** | 0.602 | **parallelism**, not compute — half the pool is idle | ~~NOT 3–5 ms: the uniform-finer-grid fix is REFUTED~~ **THIS CELL'S REFUTATION IS OBSOLETE — see §00d (2026-08-16d)** | the raster grid was a fixed **6×5 = 30 tiles** (`RENDER.CPP:449`); it is now `--frame_tile_x/y`, still defaulting to 6×5. **The 2026-08-14 refutation ("+41 % wall, +139 % instructions, because each clipper tile re-walks the whole face list") died with `c26c1c35`+`d9dfa527`: the same 12×10 probe now costs +2.7 % instructions.** But 12×10 was also the wrong SHAPE — it buys `effPar` 2.9 → 3.4 and nothing else. **`6×20` (the same 120 tiles, spent on rows) takes `renderFrame` −11.7 % here for +0.5 % instructions.** Not landed: greets moves 16 % of its pixels for +0.4 % wall, so this is a per-scene judge call, priced in full in §00d. Also note this cell's `effPar` 5.5 is stale — §00c took 57 → 26 core-ms out of the phase without moving the barrier, so it reads **2.9** today |
| 10 | **`lighting-w2`** (checkerboard fill wave 2) | greets t=5743 flat | **3.2** | 0.498 | compute; per-symbol `Render_DeferredLighting_TileFill` 7.3 % | 0.5–1 ms | `TileFill` is always scalar and its fallback replays the full wave-1 kernel. Turning checkerboard OFF costs 53.1 → 79.3 ms — do not propose that |
| 11 | `shadow-bake` | greets t=5743 | 2.4 | 0.205 | compute, `effPar` 8.3/12 | — | mature |
| 12 | `bloom-chain` | greets t=5743 | 1.78 | 0.220 | compute, IPC 4.28 | — | unattacked; sat outside every timer until 2026-08 |
| 13 | mirror `RTT` | greets t=5743 | 1.53 (flat) / 2.09 (displaced) | 0.018 | — | — | 0.00 ms in §0; it is nonzero now because `7953bab5` moved `setDefault(mirror_rtt)` into the INIT block. Expected, priced, not a defect |
| 14 | `mirror-grid` | greets t=5743 | 0.67 | — | full-res scalar scan of the mirrorMask plane, every frame | 0.3–0.5 ms | still unattacked since the backlog flagged it |
| 15 | `gbuf-clear` | city t=1961 | 0.75 | 0.009 | **memory** — IPC **0.61**, the one bandwidth-bound phase in the tree | — | the instrument's own control: it proves IPC discriminates rather than reading 3.5 everywhere |

### Per-scene phase tables

**greets t=5743, 1920×1080, load 3.5→18.9.** Arms: flat / `--greets_displace` /
+`--greets_displace_free_edge --greets_displace_border_mean=2 --greets_displace_seam_weld`
/ + `--mip_aniso --texture_filter=1`.

| phase | flat | disp | dispfull | dispfull+mip |
|---|--:|--:|--:|--:|
| **frame min** | **55.59** | **57.99** | **58.69** | **59.94** |
| `renderFrame` | 48.26 | 49.15 | 49.40 | 50.48 |
| `gbuffer` | 5.97 | 9.93 | 9.76 | 11.79 |
| `DeferredLighting-call` | 34.94 | 32.73 | 32.26 | 31.84 |
| ⤷ `lighting-w1` | 29.35 | 27.55 | 27.22 | 26.63 |
| ⤷ `lighting-w2` | 3.22 | 3.07 | 3.08 | 3.07 |
| `cones` | 1.24 | 1.24 | 1.48 | 1.44 |
| `bloom-chain` | 1.78 | 1.76 | 1.76 | 1.76 |
| `tonemap-post` | 0.69 | 0.69 | 0.70 | 0.69 |
| `TBR-render` | 0.58 | 0.49 | 0.51 | 0.48 |
| `mirror-grid` | 0.67 | 0.71 | 0.71 | 0.70 |
| `gbuf-clear` | 0.34 | 0.35 | 0.35 | 0.35 |
| `hdr-begin` | 0.26 | 0.26 | 0.26 | 0.26 |
| — outside `renderFrame` — | | | | |
| `shadow-bake` | 2.41 | 2.44 | 2.47 | 2.37 |
| `RTT` | 1.53 | 2.09 | 2.19 | 2.14 |
| `Tick-Light` | 0.30 | 0.86 | 0.90 | 0.91 |
| `Tick-Xfrm` | 0.26 | 0.45 | 0.48 | 0.49 |
| `Tick-Radix` | 0.07 | 0.28 | 0.30 | 0.30 |
| `Ginstr/f` (`renderFrame`) | 4.964 | 5.44 | 5.54 | 5.72 |
| `IPC` (`lighting-w1`) | 3.51 | 3.55 | 3.55 | 3.56 |
| `effPar` (`lighting-w1`) | 11.4 | 11.4 | 11.5 | 11.4 |

**The displaced arm shades FASTER and rasterises SLOWER.** `gbuffer` +3.96 ms,
`lighting-w1` −1.80 ms. Displaced stone puts more geometry through the raster and
changes which materials cover the screen; the net at this pose is +2.40 ms.

**greets t=3122, 1512×848, `FDS_GREETS_CAM="-8.6249094,…"` — the user's real
workload.** Load 10.4→7.6.

| phase | flat | disp | dispfull | dispfull+mip |
|---|--:|--:|--:|--:|
| **frame min** | **50.97** | **54.32** | **54.71** | **54.69** |
| `renderFrame` | 43.85 | 45.14 | 44.85 | 44.79 |
| `lighting-w1` | 17.95 | 18.86 | 17.76 | 16.92 |
| **`cones`** | **7.11** | 7.16 | 6.89 | 6.49 |
| **`TBR-render`** | **6.15** | 6.25 | 6.93 | 6.44 |
| `gbuffer` | 4.02 | 4.55 | 4.53 | 5.62 |
| `RTT` | 2.05 | 2.86 | 2.78 | 2.81 |
| `lighting-w2` | 1.96 | 2.17 | 1.94 | 2.05 |
| `bloom-chain` | 1.22 | 1.26 | 1.21 | 1.27 |
| `Ginstr/f` (`renderFrame`) | 5.074 | 5.116 | 5.116 | 5.210 |

His pose is a **different frame from t=5743**: cones 7.1 ms and TBR 6.2 ms
(1.185 Ginstr — the shards) are first-class items here and near-zero at t=5743.
Anyone tuning "the greets frame" against t=5743 alone is tuning the wrong frame
for him.

**greets t=6001 — the corner pose, where the new band/weld machinery lands.** Load 7.6→8.7.

| phase | flat | disp | dispfull | dispfull+mip |
|---|--:|--:|--:|--:|
| **frame min** | **44.74** | **46.89** | **51.24** | **52.28** |
| `renderFrame` | 39.80 | 41.07 | 45.30 | 46.57 |
| `gbuffer` | 5.36 | 7.87 | 9.00 | 10.59 |
| `lighting-w1` | 23.63 | 22.68 | 24.88 | 25.03 |
| `cones` | 0.02 | 0.02 | 0.20 | 0.20 |
| `Ginstr/f` (`renderFrame`) | 4.175 | 4.488 | 4.575 | 4.742 |

**city t=1961 / t=2400 / t=400, `--deferred`.** Load 2.9→8.7. `renderFrame` runs
**twice** per city frame; `cones-call` once.

| phase | t=1961 | t=2400 | t=400 |
|---|--:|--:|--:|
| **frame min** | **86.94** | **50.59** | **62.96** |
| `RNDR` | 72.14 | 44.89 | 55.22 |
| `renderFrame` (×2) | 62.75 | 36.67 | 46.93 |
| **`cones-call`** (×1) | **16.65** | 9.05 | 13.20 |
| `DeferredLighting-call` (×2) | 12.35 | 5.40 | 9.08 |
| **`fastfog`** (×1) | **9.98** | 5.55 | 6.29 |
| `gbuffer` (×2) | 9.85 | 5.97 | 5.08 |
| `TBR-render` (×2) | 7.49 | 5.52 | 7.96 |
| `gbuf-clear` | 0.75 | 0.72 | 0.72 |
| `ANIM` | 4.30 | 3.27 | 4.66 |
| `Ginstr/f` (`renderFrame`) | 6.366 | 3.418 | 4.925 |
| `Ginstr/f` (`cones-call`) | 2.257 | 1.222 | 1.691 |

> **`--deferred` is REQUIRED to bench city, fountain and chase.** greets forces the
> deferred path from inside the scene (`greets_mirror` → `Render(ForceDeferred)`);
> the others do not, so a bench line without the flag silently profiles the
> **forward** renderer — no cones, no `DeferredLighting`, no `fastfog`. It reads
> as a 9.3 ms city `renderFrame` and it is not the shipping path. Caught here by
> the missing phases; recorded so the next round does not lose a batch to it.

**chase t=800 / t=1600, `--deferred`.** chase has **no `--bench=scene` arm**
("scene 'chase' not supported") and **no `--repro` wiring**, so it is profiled by
asking the snapshot harness for the same timestamp ten times — the driver
re-ticks and re-renders each one and `--deferred_prof`'s warmup exclusion drops
the cold frame exactly as under `--bench`.

| phase | chase t=800 | chase t=1600 | fountain t=2500 | fountain t=1200 |
|---|--:|--:|--:|--:|
| **frame min** | — | — | **26.51** | **30.73** |
| `renderFrame` | 46.88 (×2) | 20.91 (×2) | 23.03 | 28.12 |
| **`cones-call`** | **21.60** | 3.41 | 0.00 | 0.00 |
| `gbuffer` | 11.30 | 5.76 | 2.64 | 1.57 |
| `DeferredLighting-call` | 6.81 | 3.64 | 1.92 | 3.31 |
| **`TBR-render`** | 2.87 | 5.00 | **16.70** | **21.69** |
| `gbuf-clear` | 0.73 | 0.75 | 0.36 | 0.35 |
| `Ginstr/f` (`renderFrame`) | 4.737 | 1.915 | 2.569 | 3.553 |
| `Ginstr/f` (`TBR-render`) | 0.490 | 0.860 | 2.228 | **3.032** |
| `IPC` (`TBR-render`) | 5.10 | 5.16 | 4.14 | 4.19 |
| `effPar` (`gbuffer`) | **5.5** | **5.5** | **5.2** | 9.5 |

chase's historical description as a FACE-dominated front end no longer holds:
**it is a cone scene**, and its second-largest cost is water glints.

### Per-symbol (Instruments Time Profiler, running samples, self time, DEMO only)

Shares are of DEMO's own running self-time over the whole recorded run, so they
include init (`stbi__do_zlib`, `stbi__create_png_image_raw`, `Initialize_*` are
texture load, not frame work). Read the *ratios between frame symbols*.

| greets t=5743 flat | share | city t=1961 | share |
|---|--:|---|--:|
| `Render_DeferredLighting_Tile` | **35.0 %** | `Render_VolumetricCones_Tile` | **28.5 %** |
| `CubeShadow_Sample` | **16.1 %** | `Render_DeferredLighting_Tile_OuterVec` | **15.1 %** |
| `Render_DeferredLighting_TileFill` | 7.3 % | `meka::TileRasterizer::apply_exact<false>` | 5.3 % |
| `meka::TileRasterizer::apply_exact<true>` | 7.0 % | **`pwater::waterWaveSlope`** | **5.2 %** |
| `resolveCubeAtten` | 6.1 % | `FrustumClipper::Render` | 4.5 % |
| `meka::TileRasterizer::apply_exact<false>` | 4.2 % | **`pwater::RenderGlints` λ** | **4.4 %** |
| `computeMapShadowAtten` | 4.0 % | `FastFog_SampleGrid` | 4.3 % |
| bloom `hdrDispatchRows` | 2.1 % | `Render_DeferredTransparentLighting_Tile<0>` | 4.2 % |
| `Render_VolumetricCones_Tile` | 1.8 % | `Froxel_CompositePixel` | 4.1 % |
| `Render_TonemapToVPage` λ | 1.4 % | `vFogNoise` | 3.9 % |
| `MekaleleShadowDepth` | 1.4 % | `Render_DeferredFastFog` λ$_3 / λ$_2 / λ$_1 | 2.9 / 2.0 / 1.0 % |
| `Transform_Objects` | 1.2 % | `CityScene::updateRippleDispMap` λ | 1.8 % |

| chase t=800 | share | fountain t=1200 | share |
|---|--:|---|--:|
| `Render_VolumetricCones_Tile` | **46.6 %** | `Render_DeferredTransparentLighting_Tile<0>` | **28.2 %** |
| **`pwater::RenderGlintsVaried` λ** | **16.9 %** | `Render_DeferredTransparentLighting_Tile<1>` | **20.9 %** |
| `Render_DeferredLighting_Tile` | 8.5 % | `meka::TileRasterizer::apply_exact<false>` | 16.6 % |
| `FrustumClipper::Render` | 5.2 % | `Render_DeferredLighting_Tile_OuterVec` | 11.6 % |
| `Render_DeferredTransparentLighting_Tile<0>` | 5.1 % | `SpriterRT<32>` | 5.8 % |
| `meka::TileRasterizer::apply_exact<false>` | 4.4 % | `barry::TileRasterizer<Blend1,Tex0,…>` | 2.0 % |
| `Render_DeferredFogPass` λ | 2.7 % | `meka::TileRasterizer::apply_exact<true>` | 1.5 % |
| `computeMapShadowAtten` | 1.8 % | `FrustumClipper::Render` | 1.4 % |

Three of these had never been per-symbol profiled: **city's `fastfog`** (it is
noise + froxel composite + three tile lambdas, not one kernel), **city's
`DeferredLighting`** (one `_OuterVec` monolith at ~100 % self — per-symbol
bottoms out, ablation is the only way inside), and **fountain's `TBR`** (two
template instantiations of the transparent lighting kernel, one per layer).
`Shadow_MaterialSkipsCasting`, 4 % of the lighting stage in the 2026-08-10
profile, is **gone** — `f481db36` hoisted it to a per-matID bitmask and the
symbol no longer appears.

### Inside the greets lighting wave — ablation re-run on today's tree

greets t=5743 flat, load 12.8→9.8, min-of-6. Same method as §0; the shadow diet
and the packed planes have landed since, so these supersede §0's split.

| arm | `renderFrame` | `lighting-w1` | `lighting-w1` Ginstr/f |
|---|--:|--:|--:|
| base | 47.80 | **29.78** | 3.291 |
| `--prof_no_lights` | 27.67 | 9.88 | 0.801 |
| `--no-shadows` | 38.13 | 20.66 | 2.132 |
| `--prof_no_cube_tap` | 40.65 | 23.00 | 2.380 |
| `--shadow_polyid_no_pcf` | 45.51 | 27.65 | 3.064 |
| `--prof_no_spec` | 46.33 | 28.48 | 3.069 |
| `--prof_no_tex` | 47.46 | 29.27 | 3.271 |
| `--no-env_refl` | 47.48 | 29.37 | 3.242 |

| component | ms | Ginstr | share of `lighting-w1` | vs §0 (2026-08-08) |
|---|--:|--:|--:|---|
| omni loop (whole) | **19.91** | 2.490 | 67 % | 19.0–21.25 → **unchanged** |
| ⤷ shadow sampling | 9.12 | 1.159 | 31 % | 10.80 → **−1.7 (the diet + packed planes)** |
| ⤷⤷ cube tap alone | 6.79 | 0.911 | 23 % | 10.28 (2026-08-10) → **−3.5** |
| ⤷⤷ the 3 extra PCF taps | 2.14 | 0.227 | 7 % | 1.80 → +0.3 |
| ⤷ specular | 1.30 | 0.222 | 4 % | 3.07 → **−1.8** |
| non-light remainder | **9.88** | 0.801 | 33 % | 8.26–8.29 → **+1.6, the one interior slice that grew** |
| albedo gather | 0.52 | 0.020 | 2 % | 0.71 → −0.2 |
| env reflection | 0.41 | 0.049 | 1 % | not measured before |

### Tessellation / displacement flag costs — per-flag, MEASURED

greets t=5743, one batch, load 8.8→12.8 (so compare **within** the table). The
instruction column is the load-robust one.

| arm | frame min | `renderFrame` | `renderFrame` Ginstr/f | Δ instr vs `disp` |
|---|--:|--:|--:|--:|
| `--greets_displace` | 61.68 | 52.07 | 5.445 | — |
| + `--greets_displace_free_edge` | 63.13 | 53.13 | 5.521 | **+1.4 %** |
| + `--greets_displace_border_mean=2` | 62.02 | 52.64 | 5.443 | **0.0 %** |
| + `--greets_displace_seam_weld` | 61.66 | 52.36 | 5.445 | **0.0 %** |
| `--no-greets_displace_groove_shade` | 62.06 | 52.63 | 5.426 | −0.3 % |
| `--no-greets_displace_mitre` | 62.29 | 52.84 | 5.441 | −0.1 % |
| full (free_edge + bmean=2 + seam_weld) | 63.17 | 53.47 | 5.536 | **+1.7 %** |
| full + `--mip_aniso` | 63.26 | 53.27 | 5.545 | +1.8 % |
| full + `--texture_filter=1` | 64.85 | 55.18 | 5.724 | **+5.1 %** |

* **`--greets_displace_free_edge` is the only per-frame cost in the family** —
  +1.06 ms `renderFrame`, +1.4 % instructions, and it is *geometry*: it frees
  7 908 border verts and densifies the border profile by +7 017 verts, which the
  raster then pays for (`gbuffer` +0.36 ms, `cones` +0.15 ms).
* `border_mean`, `seam_weld`, `groove_shade` and `mitre` are **per-frame free**
  — they are bake-time shape decisions. The ±0.3 % readings are noise.
* **`--mip_aniso` is free at this pose** (+0.02 % instructions), consistent with
  the +0.12 % recorded at t=5970. It costs at the fan-split sites only.
* **`--texture_filter=1` still costs and still does not pay** — +1.7 ms, +5.1 %
  instructions, all of it in `gbuffer` (10.59 → 12.33). §0's adjudication holds
  on today's tree; do not re-propose it as a perf lever.
* Whole-family cost of tessellation at the three poses, flat → full arm:
  **t=5743 +3.10 ms, his pose +3.74 ms, t=6001 +6.50 ms.**

### Scene-init cost of the displacement bake — the veto-soup worry, priced

`--init_timeline`, min over 5 rounds, greets-entry path.

| mark | flat | `--greets_displace` | full arm | recorded 2026-08-09 |
|---|--:|--:|--:|--:|
| `Initialize_Greets` total | **1 358** | **1 976** | **2 026** | 1 379 flat / 2 469 displaced |
| ├ `DisplaceStoneSubdiv` block | 0 | **440** | **483** | **573** |
| ├ `MakeFacesIndependentByAngle` | 4.5 | — | 54 | — |
| ├ chunking / clustering | 3.6 | — | 77 | — |
| ├ `ShadowMaps_BakeStatic` | 10.7 | — | 32 | — |
| └ shadow/env bake | 13.8 | — | 49 | — |

**VERDICT: the init did not regress; it improved.** The subdivision block is
**483 ms against the 573 ms recorded** before the weld/band/blend/veto machinery
existed, and `Initialize_Greets` is **2 026 ms against 2 469 ms**. The full
machinery — free-edge densification, the abuttal veto's scene-wide face soup at
5 samples per candidate edge, the mitre weld, the corner-band blend — adds
**+43 ms** over plain `--greets_displace` (440 → 483). The O(edges × faces) fear
is not borne out at this scene's size. What the displaced arm actually pays at
init is **downstream** of the subdivision: `MakeFacesIndependentByAngle`
+50 ms, chunking/clustering +74 ms, the static shadow bake +21 ms, the env bake
+35 ms — all of them proportional to the face count the subdivision produced.

### REGRESSION HUNT — a real parent-binary A/B, not a cross-session ms comparison

Parent `a3a72cc5` (2026-08-13 22:25, the tip of cone round 5) against HEAD
`a9ca03dc`, **both binaries built in one worktree and run against one asset tree**
(`git diff a3a72cc5 a9ca03dc -- Runtime/` is empty, so this is a pure code A/B),
interleaved round-robin, min-of-6, load 24.6→13.2. The window spans every
2026-08-14 landing: the mitre weld, the band pre-split, the corner-band blend,
groove shading, the edge-notch densification, free_edge rounds 4–5, and the env
water mask.

| arm | parent frame | HEAD frame | Δ | parent Ginstr/f | HEAD Ginstr/f | Δ instr |
|---|--:|--:|--:|--:|--:|--:|
| greets t=5743 flat | 55.47 | 55.50 | **+0.03** | 4.964 | 4.962 | **0.0 %** |
| greets t=5743 `--greets_displace` | 63.15 | 62.96 | −0.19 | 5.423 | 5.443 | +0.4 % |
| **greets t=6001 full displace arm** | **43.49** | **50.02** | **+6.53** | **4.455** | **4.577** | **+2.7 %** |
| greets t=3122 his pose `--greets_displace` | 54.33 | 54.03 | −0.30 | 5.112 | 5.115 | 0.0 % |
| city t=1961 `--deferred` | 86.10 | 85.08 | −1.02 | 6.365 | 6.370 | 0.0 % |

**One regression, and it is geometry, not code: +6.53 ms at the corner pose in
the full displacement arm.** `gbuffer` carries it — 7.34 → 8.77 ms wall and
0.947 → 1.020 Ginstr/f (+7.7 %) — with `lighting-w1` +3.24 ms and `cones`
0.02 → 0.20 ms behind it. The 08-14 commits generate more geometry at the corner
than the parent's same flags did, which is what they were written to do; nobody
priced it. **Everything else in the window is perf-null**, including the flat
arm, the plain displaced arm, the user's pose and city.

**The shatter bracket does NOT regress.** `--repro=greets@t=3122
--repro_from=3112 --repro_settle=0`, `FDS_GREETS_SHATTER=1`,
`FDS_GREETS_CAM="28.8,10.8,-62.85,1,0,0"`, `FDS_SHARD_REFL_PROF=1`, second
shatter frame, **min-of-8**:

| | parent | HEAD | recorded anchor |
|---|--:|--:|--:|
| shard bake wall | 12.0 ms | **11.7 ms** | **11.5 ms** |
| `Render_DeferredLighting` core-ms | 99.8 | **94.6** | **94.3** |

The anchor reproduces. **Method note that cost me a wrong answer first:** the
same bracket read **16.5 ms** at min-of-6 in a two-arm batch at load ~8 and
**11.7 ms** at min-of-8 — this measurement needs 8 rounds, and the campaign that
set the anchor said so (min-of-8 paired). Do not quote it from 6.

**What is NOT a regression, stated because the raw numbers look like one.**
greets t=5743 flat `renderFrame` reads **48.3 ms** here against §0's **43.65**,
and `lighting-w1` **29.4** against **27.32**. That is not code:

* the parent binary reads the **same** 48.4 ms in the A/B above, so nothing in
  the 08-13→08-14 window did it;
* `lighting-w1` Ginstr/f is **3.290**, against the **3.271** the shadow diet
  left it at on 2026-08-12 — the load-robust column says the kernel retires the
  instructions it is supposed to;
* cycles read +7 % and wall +7.5 % on flat instructions, which is the signature
  of a busier machine, not a fatter kernel.

Likewise fountain t=2500 `renderFrame` 23.03 against §0's 20.09 and the shard
bake's first reading: same shape, same cause. **Cross-session ms comparisons on
this box are worth less than the Ginstr column; when they disagree, believe the
counter.**

### Levers this round did NOT find

* No new memory-bound phase. Every hot phase reads IPC 3.5–5.2 against a 2.42
  compute anchor. `gbuf-clear` (IPC 0.61) and `hdr-begin` (1.12) are the only
  bandwidth-bound phases and together they are 0.6 ms.
* No barrier-tail problem in greets: `lighting-w1` `effPar` is 11.4/12.
* No init regression (above).
* No trivially-safe one-liner. Nothing in this round is fixable in one line.

---

State of the engine on `feature/static-shadow-lightmaps`, gathered for invasive perf work on
the deferred kernel. Numbers measured at greets `t=500`, 1920×1080, low-poly Piramid (5.5k
faces), chunked at `--greets-piramid-chunk-grid=8`, per-cube-face cull on. Read this top-to-
bottom once; thereafter use the tables.

This is descriptive, not prescriptive. Things that look wasteful are flagged; fixes are not
proposed here.

---

## 0. MEASURED phase split of `renderFrame` — 2026-08-08

Instrument: `--deferred_prof=<warmup>` (`FDS/RENDER/TailProf.h`, see
`docs/GRAPHICS_PIPELINE.md` §8b). 1920×1080, 12 pool workers, scene defaults as
shipped (greets: PBR + HDR + bloom 2.0 + checkerboard + mirror + `cone_fine_tiles`;
city: `cine::kCity`; fountain: `cine::kFountain`).
`--bench=scene@scene=<s>,t=<T>,iters=60`, warmup 5, **min over steady frames, then
min-of-arm over 3 interleaved repetitions.**

**Load: the machine was shared for the whole campaign** (two other agents running
`./DEMO`; 1-min load average 16–57 per run, recorded per row). Serial phases
(`hdr-begin`, `mirror-grid`, `depth-bounds`, `tile-cull`) came out identical to 3
decimals across every arm and act as the internal control; the *parallel* waves are
the load-sensitive ones, which is why every comparison below is interleaved and
min-of-arm. **Sub-0.1 ms phases are not resolvable here and are not interpreted.**

`OTHER` (per-frame `renderFrame` minus Σ of its phases) is **0.048–0.056 ms on every
greets/fountain row and 0.15 ms on city** — i.e. the frame is fully attributed, which
is what makes the rest of the table quotable.

**The instrument is byte-null with the flag off** — verified, not assumed:
`tools/render_gate.sh` 3/3 PASS and the city (`e1221676`) and fountain (`8db68ccb`)
pins unchanged. The greets pin reads `6780642b` on this tree rather than `adfba8ba`,
and that is **not** the instrument: `--hdr_metal_kill=0` reproduces `adfba8ba`
byte-exactly, so the mover is that flag's default of 2 (a separate, intended look
change landed the same day — `docs/SHADING_CONTRACT.md` D1). Values 0/1/2 give
`adfba8ba` / `3d82e4b6` / `6780642b`, all stable 3/3.

### The five poses (ms/frame, `wall_min`)

| phase | greets 5743 | greets 2000 | greets 4200 | fountain 2500 | city 1961 |
|---|--:|--:|--:|--:|--:|
| **renderFrame (= essentially all of RNDR)** | **43.65** | **63.82** | **38.81** | **20.09** | **69.82** |
| G-buffer clear | 0.34 | 0.32 | 0.32 | 0.34 | 0.73 |
| **G-buffer fill (`gbuffer`)** | **5.59** | **4.77** | **5.12** | **2.51** | **9.20** |
| HDR buffer begin (33 MB f32 clear) | 0.13 | 0.13 | 0.13 | — | — |
| **deferred lighting (total)** | **32.59** | **47.14** | **29.18** | **1.74** | **10.48** |
| ⤷ light list + per-tile cull + depth bounds + mirror grid | 1.04 | 1.07 | 1.03 | 0.29 | 0.74 |
| ⤷ shading wave 1 (`lighting-w1`) | 27.32 | 35.57 | 24.25 | 1.36 | 9.38 |
| ⤷ checkerboard fill wave 2 | 3.14 | 9.20 | 3.04 | — | — |
| froxel fog (`fastfog` + sky paint) | — | — | — | — | 10.39 |
| volumetric cones (`cones-call`) | 1.22 | 6.20 | 0.53 | — | **30.72** |
| transparent peel (`xpar-peel`) | 0.08 | 0.05 | 0.05 | 0.14 | 0.25 |
| **TBR (sprites + unified transparents)** | 0.59 | 2.19 | 0.21 | **14.77** | 7.24 |
| bloom chain (DoF+bright+anam+bloom+ghosts) | 1.74 | 1.78 | 1.77 | — | — |
| tonemap + LDR post | 0.69 | 0.67 | 0.69 | — | — |
| everything else (sprite insert, prologue, overlays, edge AA) | <0.03 | <0.03 | <0.03 | 0.16 | 0.06 |
| `OTHER` (unattributed) | 0.049 | 0.051 | 0.049 | 0.053 | 0.149 |
| — outside RNDR — | | | | | |
| shadow cube bake (`BAKE` section) | 2.05 | 3.39 | 2.28 | 0 | 0 |

Notes on shape:
- city's `renderFrame` runs **twice per frame** (the water-reflection underlay plus
  the final view); the column is the per-frame total of both.
- **Mirror RTT and the shadow bake are NOT inside RNDR.** In `GREETS.CPP` the RTT sits
  in `PROF_ANIM` (~L3712) and `ShadowBake_DispatchGreets` in `PROF_BAKE` (~L3813); both
  measured 0.00 and 2.05 ms respectively at t=5743. Nothing hides in RNDR on their behalf.
- fountain is the only scene with a TBR, and it is **73 % of its frame**.

### Wall vs thread-sum (they answer different questions)

`wall` above is ELAPSED on the tick thread and sums to the frame. The parallel waves
also report `thrsum` = Σ tile-task durations (CORE-ms) and `effPar = thrsum/wall`:

| wave (greets 5743) | wall | thrsum | effPar (of 12) |
|---|--:|--:|--:|
| `lighting-w1` | 27.3 | ~370–520 | **10.2–11.4** |
| `lighting-w2` | 3.1 | ~40–49 | 5.9–11.3 |
| `gbuffer` | 5.6 | ~62–76 | 6.7–9.7 |
| `cones` | 1.2 | ~9–13 | 4.1–7.4 |

**The lighting wave is compute-bound and balanced, not barrier-tail-bound** (effPar
10–11 of 12 workers). There is no reclaimable idle there: the fix has to remove
per-pixel work, not rebalance tiles. The `gbuffer` and `cones` waves *do* leave
parallelism on the table (effPar 4–10), and `cones` runs on the coarse 6×4 grid
outside greets.

### Inside the lighting wave — ablation

`--prof_no_lights` (omni loop off) against the same-pose baseline, control phases
matched within 2–9 %:

| pose | `lighting-w1` base | `--prof_no_lights` | ⇒ omni loop | share of frame |
|---|--:|--:|--:|--:|
| greets 5743 | 27.32 | 8.28 | **19.0** | **44 %** |
| greets 2000 | 35.57 | 10.14 | **25.4** | **40 %** |
| city 1961 | 9.38 | 5.99 | 3.4 | 5 % |

So on greets the **per-light loop (cube-shadow taps + per-light PBR) is the single
largest slice of the frame**, and the remaining ~8 ms of the wave is the G-buffer
decode + material resolve + all the map fetches + ambient/SH + env compose.

Three independent sets at greets 5743 (this one, plus the mechanism and shadow sets
below, taken at different loads on different arms) put the omni loop at **19.0 / 21.25
/ 20.61 ms** — call it **~20 ms, 43–46 % of the frame**. The spread is entirely in the
baseline `lighting-w1` (27.32 / 29.50 / 28.90, load-driven); `--prof_no_lights` itself
lands at **8.28 / 8.26 / 8.29 ms across all three**, which is as tight a repeat as this
machine gives and is why the non-light remainder is quoted with confidence.

#### Splitting the omni loop — shadow sampling vs per-light shading

Tightly interleaved, greets t=5743, **3 reps each at load 22–26** (the quietest set of
the campaign; controls `hdr-begin` 0.128–0.129, `mirror-grid` 0.645–0.661,
`bloom-chain` 1.82–1.89, `gbuffer` 5.52–5.80 — all matched within 4 %):

| arm | `renderFrame` | `lighting-w1` | removes |
|---|--:|--:|---|
| base | 46.34 | **28.90** | — |
| `--shadow_polyid_no_pcf` | 44.59 | **27.10** | 3 of the 4 PCF taps |
| `--no-shadows` | 35.09 | **18.10** | all shadow sampling |
| `--prof_no_lights` | 25.85 | **8.29** | the whole omni loop |

Differencing the shading wave:

| component | ms | % of `lighting-w1` | % of the 46.3 ms frame |
|---|--:|--:|--:|
| everything before/around the light loop (G-buffer decode, matID→Material\*, normal/metal/rough/AO/horizon fetches, ambient + SH, env compose, store) | **8.29** | 29 % | 18 % |
| per-light shading math, shadows excluded (range/cone tests, attenuation, GGX/Fresnel, accumulate) | **9.81** | 34 % | 21 % |
| shadow sampling (cube taps + lightmap + spot maps) | **10.80** | 37 % | **23 %** |
| ⤷ of which the 3 extra PCF taps | 1.80 | 6 % | 4 % |

The 2026-05 estimate at the top of this document put "per-pixel cube-shadow taps" at
~32 ms and called it the #1 cost; on today's content and defaults it is **10.8 ms** and
it is roughly TIED with the per-light shading math it sits inside. The headline has
changed: no single stage owns the greets frame — shadows 23 %, per-light math 21 %,
the rest of the kernel 18 %, the checkerboard fill wave 7 %, the G-buffer fill 12 %.

### `--texture_filter` 0 / 1 / 2 — MEASURED, and it does NOT pay

Hypothesis under test: the G-buffer stores a texel ADDRESS, so the kernel pays a
dependent random gather per pixel; `--texture_filter>0` makes the rasterizer write a
filtered BGRA plane the kernel reads LINEARLY, which might be a free perf win.

`renderFrame` ms/frame, min-of-arm over 3 interleaved reps:

| pose | tf=0 | tf=1 (bilinear) | tf=2 (trilinear) |
|---|--:|--:|--:|
| greets 5743 | **43.65** | 44.58 | 45.36 |
| greets 2000 | **63.82** | 65.85 | 65.87 |
| greets 4200 | **38.81** | 39.68 | 41.17 |
| fountain 2500 | 20.09 | **20.03** | 21.09 |
| city 1961 | **69.82** | 71.32 | 73.79 |

It **costs** 0.9–4.0 ms and never wins. The two phases that move say why:

| pose | `lighting-w1` tf0→tf1→tf2 | `gbuffer` tf0→tf1→tf2 |
|---|---|---|
| greets 5743 | 27.32 → 27.29 → 27.53 | 5.59 → 6.76 → 6.86 |
| greets 2000 | 35.57 → 35.52 → 35.93 | 4.77 → 6.11 → 6.23 |
| greets 4200 | 24.25 → 23.99 → 25.03 | 5.12 → 6.52 → 6.71 |
| city 1961 | 9.38 → 9.39 → 9.59 | 9.20 → 10.06 → 10.95 |
| fountain 2500 | 1.36 → 1.32 → 1.39 | 2.51 → 2.84 → 3.07 |

**The kernel does not get faster — at all, at any pose.** The mechanism: the filtered
plane replaces only the *albedo* gather. The normal, metal, roughness, AO and horizon
maps are still fetched at the same `Mipmap[miplevel][swizzledUV]` address
(`DeferredSurfaceKernel.cpp` ~1659/2519/2535/1905/1972), so the dependent address
chase and its miss pattern survive intact — one of five gathers removed changes
nothing measurable. Meanwhile the raster pass pays a consistent **+1.2–1.4 ms** to
bilinear-sample and write an extra full-res BGRA plane.

`--texture_filter` remains a QUALITY flag (it fixes texel crawl). It is not a perf
lever, and it should not be defaulted on for performance reasons.

#### The direct proof: `--prof_no_tex` makes the kernel no faster

A separate, TIGHTLY interleaved set at greets t=5743 (every arm back-to-back inside
each rep so they share the same competing load; 4 reps; control phases `hdr-begin`
0.129-0.130, `cones-call` 1.218-1.274, `TBR-render` 0.619-0.650 confirm comparability):

| arm | `renderFrame` | `lighting-w1` | `gbuffer` |
|---|--:|--:|--:|
| baseline | 46.42 | **29.50** | 5.93 |
| `--prof_no_tex` (albedo fetch -> constant) | 47.47 | **28.79** | 5.87 |
| `--texture_filter=1` | 49.13 | **29.46** | 7.30 |
| `--texture_filter=1 --prof_no_tex` | 47.51 | 28.92 | 7.23 |
| `--prof_no_spec` | 44.42 | 26.43 | 5.50 |
| `--prof_no_lights` | 25.53 | **8.26** | 5.59 |

Two rows kill candidate (C) between them:

- **The albedo gather is worth only 0.71 ms** (29.50 - 28.79 = 2.4 % of the wave,
  1.5 % of the frame). Deleting it *outright* - not replacing it, deleting it - buys
  0.7 ms. That is the entire prize the filtered-albedo plane is competing for.
- **The filtered plane does not even collect it: 29.46 vs 29.50, a 0.14 % difference**,
  while `gbuffer` pays +1.37 ms for the extra plane. Net loss by construction.

The reason is structural, not incidental: the plane replaces one of *five* fetches at
the same `Mipmap[miplevel][swizzledUV]` address - normal, metal, roughness, AO and
horizon maps all still chase it (`DeferredSurfaceKernel.cpp` ~1659 / 2519 / 2535 /
1905 / 1972). Removing one of five leaves the address chase and its miss pattern
intact.

Same set, other splits: **specular = 3.07 ms** (29.50 - 26.43, 10 % of the wave);
**omni loop = 21.25 ms** (29.50 - 8.26, **72 % of the wave, 46 % of the frame**),
which agrees with the independent 20.61 ms from the shadow set above (different arms,
different load) and whose non-light remainder agrees to 8.26 vs 8.29.

### Anchor — the `RNDR 64.017` in GPU_BENCHMARK_PLAN §6.2c, split

§6.2c's CPU column was taken at greets t=5743 under the matched-tier flags
(`--no-greets_mirror --no-mirror_rtt --no-greets_disco --no-parallax
--no-shadow_lightmap --no-deferred_checkerboard`). Re-run today on this tree
alongside the shipped-defaults arm, 3 interleaved reps:

| | shipped defaults | §6.2c tier C | §6.2c reported |
|---|--:|--:|--:|
| frame min | 50.59 | **66.67** | 67.61 |
| `RNDR` min | 46.72 | **62.93** | 64.017 |
| `BAKE` min | 3.06 | 3.04 | 2.992 |
| `renderFrame` | 46.10 | 62.80 | — |
| ⤷ `lighting-w1` | 28.83 | **56.22** | — |
| ⤷ `gbuffer` | 5.69 | 2.30 | — |
| ⤷ `lighting-w2` (checkerboard fill) | 3.35 | n/a (full rate) | — |

So §6.2c reproduces within **1.7 %**, and its 64 ms is **89.5 % one thing: the
deferred lighting shading wave** at full shading rate. (The tier's `--no-greets_mirror`
also removes the mirror clone geometry, which is why its G-buffer fill is 2.30 vs 5.69
— the clone costs ~3.4 ms of raster in the shipped configuration.)

### Per-tile light census (`FDS_TILE_LIGHT_PROF=1`), 12×8 grid = 160×135 px

| pose | view lights | avg/tile | avg/non-empty tile | max |
|---|--:|--:|--:|--:|
| greets 5743 | 117 | 6.9 | 8.3 | 39 |
| greets 2000 | 117 | 7.3 | 8.8 | 41 |
| city 1961 | 76 | 26.1 | 33.0 | 53 |

**Read this next to the omni-loop table above and it settles an argument:** city
carries **3.8× more lights per tile than greets** and its omni loop costs **3.4 ms**;
greets carries 6.9 and its omni loop costs **19–21 ms**. Whatever sets the per-light
cost, it is **not the per-tile light count** — the two move in opposite directions by
a factor of ~23. Any proposal that attacks lights-per-pixel has to get past this row
first.

I did **not** determine why the per-light cost differs so much between the two
scenes, and will not guess: the obvious candidate (greets on the scalar path, city on
the 8-wide vec path) is **wrong** — `deferred_vec` defaults OFF on arm64
(`FDS_DEFERRED_VEC_DEFAULT`, FeatureFlags.h), so both scenes run the scalar per-light
loop on this machine. Plausible remaining differences (unmeasured): how many of each
tile's lights survive the per-pixel range/cone test, how many pixels are sky
(`zEnc == 0`, skipped entirely), and how many lights carry a cube shadow map. **The
per-pixel surviving-light count is the measurement candidate (B) actually needs and
it does not exist yet** — the census above counts per TILE, not per pixel.

### `--cone_fine_tiles` on city — measured, no win

The cone pass is 30.7 ms = 44 % of the city frame and runs on the coarse 6×4 grid
there (greets defaults it to 12×8, where it was worth ~8 %). Interleaved A/B at
city t=1961, 3 reps each, load 28–37 — an unusually stable pair (`renderFrame` spread
0.8 % within each arm):

| | coarse 6×4 | fine 12×8 |
|---|--:|--:|
| `cones-call` | 30.866 | 30.967 |
| `renderFrame` | 70.837 | 71.399 |
| `RNDR` min | 79.763 | 79.136 |

**No gain — the greets result does not transfer.** The two arms are within 0.8 %,
i.e. inside the run-to-run spread. city's cone cost is not a tile-balance problem.

---

## 1. Pipeline overview — one greets frame in deferred mode

| # | Stage | File | Function | Threading | ms (greets t=500, 1080p, full shadows) |
|--:|---|---|---|---|--:|
| 1 | Splines + animation | `FDS/RENDER/Transform.cpp` | `Animate_Objects` | single | ~0 |
| 2 | Per-frame greets driver (robot spot tracking, orbit spots) | `DEMO/GREETS.CPP` | inline in `Tick_Greets` (~L1732+) | single | ~0 |
| 3 | Main camera transform/cull | `FDS/RENDER/Transform.cpp` | `Transform_Objects(GreetSc, g_mainCamera, g_mainFaces)` | per-mesh thread tasks | ~few |
| 4 | Lighting (per-vertex, forward) | `FDS/RENDER/Lighting.cpp` | `Lighting(GreetSc)` | single | low — deferred path skips most of it |
| 5 | Radix sort of FList | `FDS/SORTS` | `Radix_Sort` | single | low |
| 6 | Main rasterize (G-buffer fill) | `FDS/RENDER/RENDER.CPP` L344-358 → `RenderInnerMekalele` → `Mekalele.h::Mekalele` | tiled 6×4 over threadpool | included in baseline-no-shadow |
| 7 | Dynamic-omni shadow bake | `GREETS.CPP:1850` → `FDS/RENDER/Shadows.cpp::Render_DeferredShadowMaps(_, DynamicOmnisPerFrame)` | per-light task (Phase A) + light×tile task (Phase B) | ~12.5 ms |
| 8 | Dynamic-mesh-into-static bake | `GREETS.CPP:1856` (gated on `--shadow-dynamic`) | as above, mode `DynamicMeshesPerFrame` | ~0 (off by default in measurements above) |
| 9 | Deferred lighting kernel | `FDS/RENDER/DeferredLighting.cpp::Render_DeferredLighting` (L3032) | tiled 12×8 over threadpool | ~44 ms (12 ms "other" + 32 ms cube tap) |
| 10 | Deferred skybox | same file, `Render_DeferredSkybox` | gated `--deferred-skybox` (off by default) | 0 |
| 11 | Deferred fog | `DeferredLighting.cpp::Render_DeferredFogPass` (L5411) | tiled 6×4 | ~small (Scn_Fogged) |
| 12 | Transparent peel (per mesh, 2-layer G-buffer) | `RENDER.CPP::renderFrame` L431-530 | per-mesh batches → tiled raster + tiled lighting | small for greets |
| 13 | Particles / sprites | inline in renderFrame L534-555 + `TBR_Render` | single + tile | low |
| 14 | Volumetric (cones + halos OR unified) | `DeferredLighting.cpp::Render_VolumetricCones`/`_OmniHalos`/`_DeferredVolumetric` | tiled | depends on flags |
| 15 | Lightmap viz overlay | `FDS/RENDER/LightmapBake.cpp::Render_LightmapViz` | single | 0 when off |
| 16 | Shadow-map debug overlay | `FDS/FILLERS/ShadowMap.cpp::ShadowMap_Overlay` | single | small |
| 17 | SDL flip | `DEMO/SDL2.cpp` | `MainSurf->Flip` → `SDL_UpdateTexture`+`SDL_RenderCopy`+`SDL_RenderPresent` | single | ~vsync |

Baseline-no-shadow = stages 1–6 + 9–17 with `prof_no_cube_tap` and `--no-shadows`. Stage 9 is
still ~12 ms even without any cube taps (see §2). The 33 ms baseline is roughly:

| Component of the 33 ms baseline | rough share |
|---|--:|
| Per-pixel deferred kernel "other" (texel fetch + ambient + omni loop + spec + sat + modulate + store) | ~12 ms |
| G-buffer fill (Mekalele rasterize over 5.5k Piramid + robot faces) | ~few ms |
| Main `Transform_Objects` + sort | low single-digit |
| Fog pass (Scn_Fogged) | low single-digit |
| Volumetric (cones/halos default on for greets) | a few ms — depends on cone_strength / halo_strength |
| Skybox/forward sprite + flip + present | rest |

The 1920×1080 framebuffer is 2.07 Mpx. Even an empty per-pixel pass that just stores 4 bytes
is ~3-4 ms at memory bandwidth, so ~12 ms of "kernel other" is plausibly:
texel gather (random reads into Material->Txtr->Mipmap[mip]) + ambient compute + Mekalele
mat32-decode + ZPage decode + (tiny) per-tile omni loop + spec for the lit pixels +
saturate/modulate/store. The texel-fetch cache miss pattern is the largest single piece.

## 2. The deferred lighting kernel

Entry: `Render_DeferredLighting()` at `DeferredLighting.cpp:3032`.

Per frame the entry does:
1. Build view-space omni list `ViewLightsSoA lights` (L3083-3174). Walks `Sc->OmniHead`,
   transforms each `Omni::IPos` into view space, copies color×ISize, range², 1/range, spot
   cone, world position, `shadowMapIdx`, `cubeShadowIdx`, halo params. Capped at
   `DEFERRED_MAX_LIGHTS=128`.
2. `computeTileDepthBounds` + `buildTileLightLists` populates 12×8 `TileLights` (L3192-3199).
   Each tile gets a compacted SoA of omnis whose screen bounding circle overlaps it.
3. (Optional) `buildStripLightLists` for unified-TBR transparent path.
4. Wave 1: 96 tile jobs into `Render_DeferredLighting_Tile` (L953) or
   `Render_DeferredLighting_Tile_OuterVec` (L2193), selected by `deferredLightingOuterVecEnabled()`.
5. Wave 2 (only if `checkerboard` or `quarter` on): 96 tile jobs into
   `Render_DeferredLighting_TileFill` (L2728).

### Two kernel implementations

| Aspect | Scalar `_Tile` (L953) | OuterVec `_Tile_OuterVec` (L2193) |
|---|---|---|
| Outer loop | scalar pixel-by-pixel | 8 px per row in `__m256` |
| Inner omni loop | scalar with branch-predicted early-out OR (when `deferred_vec=1`) 8 omnis-per-pixel vec body L1268-1399 | 1 omni × 8 pixels per iter L2510-2566 |
| Texel fetch / matTable / ambient | scalar | scalar per-lane (no vgather on arm64/simde) |
| Normal decode (oct → x/y/z) | scalar `meka::oct_decode_u16` | 8-wide via the inline code L2360-2391 |
| Normal-map (TBN) | scalar block L1108-1152 | per-lane scalar L2400-2457 — forces lanes through scalar fallback in the omni loop |
| Spec | per-omni `pow_glossClass` (template ladder by gloss) | spec lanes go through full **scalar fallback** L2612-2694 — the OuterVec omni loop is short-circuited for them |
| Water | special case `isWater` skip + half-blend | same lanes go through scalar fallback |
| 2D spot shadow tap | scalar inline at L1521-1640 | NOT in OuterVec vec body; only reached via scalar fallback (and the fallback inside L2612-2694 does not run the 2D shadow tap either — see §8 quirk) |
| Cube shadow tap (`resolveCubeAtten`) | scalar L1646-1657 (called once per omni per pixel) | scalarized lane-by-lane L1349-1378 *inside* the inner-vec body of the standard tile when `deferred_vec=1`, plus the OuterVec spec fallback omits cube tap entirely (see §8) |

`deferredLightingOuterVecEnabled()` (L737-744) tri-state:
- Explicit CLI/env `--deferred-outer-vec` wins.
- Else `CurScene->PreferOuterVec`.
- City / Crash / Fountain set it to 1 (matte/spec-light scenes win).
- Greets does NOT set it → defaults to 0 → greets runs the **scalar `_Tile`**.

This matches memory tag `project_outervec_incorrect`: OuterVec matches std visually (0.42%
pixel diff) but is ~2 ms slower on greets because most greets pixels are spec+nmap → scalar
fallback path. Off by default for greets.

### Per-pixel stages in scalar `_Tile` (the actual greets kernel)

Reading the code top-to-bottom in `Render_DeferredLighting_Tile`, every alive pixel goes
through (assuming default flags + matte path):

| Step | Lines | What happens | SIMD? |
|---|---|---|---|
| Wave-skip test | 998-1001 | early `continue` for odd cells if `checker`/`quarter` | scalar |
| Z decode | 1003-1006 | one load `ZPage16[i]`, branch on `zEnc==0` | scalar |
| mat32 decode | 1008-1011 | one load, 3 shifts | scalar |
| matTable bounds + Material* + Txtr null guards | 1012-1014 | one indirection | scalar |
| `surfaceShadowId` decode | 1023-1025 | one load `gb.shadowMatID[i]` (16b) or fallback | scalar |
| Texel fetch | 1042-1054 | indirect `Mipmap[mip][swizzledUV]` | scalar; **single biggest L1/L2 miss source** |
| Lightmap resolve (`resolvePixelLightmap`) | 1059 → L786 | reads `lightmapMF[i]`+`lightmapST[i]`, indexes scene `staticLMTable` | scalar; quick fall-through when off |
| Normal decode | 1069-1070 | `oct_decode_u16` | scalar |
| Normal-map TBN | 1090-1152 | only when `Mat->NormalMap` ≠ null; reads tangent G-buffer, TBN matmul, renormalize | scalar |
| Reconstruct view-space pos | 1157-1159 | `(0xFF80-zEnc)*invZScale` + projection | scalar |
| Ambient | 1163-1172 | Lumin + Diffuse×Ambient per channel | scalar |
| View dir (for spec) | 1196-1203 | `-pos / |pos|` via `fast_rsqrt` — only if `wantSpecular` | scalar |
| World-pos reconstruct | 1242-1250 | `viewToWorld * pos + cameraWorld` — needed for cube tap | scalar |
| Omni loop | 1252-1697 | one of: scalar branch-predicted (L1480+), vec 8-omnis (L1267+), or skip if `profNoLights`/`isWater` | scalar by default; vec only when `--deferred-vec` (off — slower on arm64) |
| **per-omni** Spot cone | 1499-1506 | scalar smoothstep | |
| **per-omni** 2D shadow tap | 1521-1640 | scalar 2×2 PCF on `sm.depth`+`sm.depth_dynamic`; OR polyId identity test on `sm.polyId`+`sm.polyId_dynamic` | scalar |
| **per-omni** Cube shadow tap | 1646-1657 | `resolveCubeAtten` → lightmap atlas (if applicable + `--shadow-lightmap`) else `CubeShadow_Sample` 4-tap PCF | scalar |
| **per-omni** Spec accumulate | 1666-1695 | half-vector + `pow_glossClass` | scalar |
| Saturate | 1701-1706 | 6 cmps | |
| Modulate | 1721-1726 | `tex*lit/256` | |
| Water composite | 1738-1746 | reads `out[i]` and half-adds | |
| Viz overrides | 1754-1772 | `--viz-normal` / `--viz-tangent` (dev) | |
| Final saturate + store | 1774-1781 | one `out[i] = ...` | |

So a typical greets pixel pays: 1 ZPage load, 1 mat32 load, 1 shadowMatID load, 1 normal
load, 0–2 nmap reads, 1 lightmap-G-buffer-pair load, 0–2 tangent loads, 1 ambient compute,
3 viewToWorld FMA chains, then for each omni in the tile list: ~6 FMAs (Lambertian) +
optional cone smoothstep + optional shadow tap (cube or 2D) + optional `pow_glossClass`.

### Shadow attenuation paths

| Light type | Path | Where | Cost |
|---|---|---|---|
| 2D spot shadow (Light_SpotLight + Omni_CastsShadow) | Scalar inline L1521-1640 (NOT in OuterVec vec body, NOT in TileFill, NOT in OuterVec spec fallback) | per-pixel-per-omni | 4 depth taps + bilinear weights + bias math |
| 2D spot polyId mode | L1579-1600 — replaces depth compare with id-not-equal-and-non-zero | per-pixel-per-omni | 4 polyId taps + identity tests |
| Cube tap (Light_Omni + Omni_CastsShadow + has cube ref) | `resolveCubeAtten` L813-951 | per-pixel-per-omni | as below |
| Cube + lightmap (Light_Omni + Omni_CastsShadow + Omni_StaticShadow + pixel on static mesh + `--shadow-lightmap`) | `pl.lm->sampleBilinear`/`sampleBilinearPlanar`/`sampleNearest` from `StaticShadowLightmap.h` (~L130-220) | per-pixel-per-omni | 4 atlas taps, bilinear blend, no cube projection |
| Cube + lightmap × dynamic (above + `--shadow-dynamic`) | composite: lightmap atlas × `CubeShadow_Sample(dynamicOnly=true)` | per-pixel-per-omni | 1 atlas tap + 1 dynamic cube tap |

**Critical**: the OuterVec kernel does NOT do 2D shadow taps in its vec body. The 2D shadow
attenuation only fires in the scalar `_Tile` and in the per-pixel branch reached via the
`deferred_vec=1` inner-vec path inside that scalar tile fn. The OuterVec's scalar fallback
(L2612-2694 for spec/water lanes) does the omni loop but **skips both shadow attenuation
calls entirely**. Cube shadows are present in `_Tile`'s standard scalar omni loop (L1646)
and the standard vec body (L1349-1378), but absent in OuterVec's omni loop and absent in
OuterVec's scalar fallback. This is consistent with `project_outervec_incorrect` note that
OuterVec matches std visually only on scenes without shadows.

## 3. Sub-pixel sampling modes

CLI flags: `--deferred-checkerboard` and `--deferred-quarter`. Env: `FDS_DEFERRED_CHECKERBOARD`,
`FDS_DEFERRED_QUARTER`. Both default off. Defined in `FeatureFlags.def` L22-23.

`deferredLightingQuarterEnabled()` wins when both are set (L767, L964).

### Checkerboard

Wave 1: `((px ^ py) & 1) == 0` → only even cells shaded. L1000 (scalar `_Tile`), L2261-2270
(OuterVec — uses lane mask).

Wave 2: `Render_DeferredLighting_TileFill` (L2728+) handles odd cells:
- For each odd pixel:
  - Left neighbor's matID and Right neighbor's matID compared against center matID (L2823-2825).
  - If both match: dword-level half-blend `((pL & 0xFEFEFEFE) >> 1) + ((pR & 0xFEFEFEFE) >> 1)`.
  - Else: full fallback shade (replays the wave-1 kernel for this pixel, L2841+).

Note: only the L/R neighbor pattern is implemented for checkerboard; no diagonal/vertical
forms.

User reports this looks acceptable.

### Quarter

Wave 1: `((px | py) & 1) == 0` → only `(even, even)` cells shaded. L1001 + L2272-2279.

Wave 2 patterns (L2754-2816), each requires all neighbors share matID with center; else
fallback:

| Cell parity | Pattern | Taps | Blend |
|---|---|---|---|
| odd_x, even_y | horizontal | `out[i-1]`, `out[i+1]` | `(pL>>1)+(pR>>1)` |
| even_x, odd_y | vertical | `out[i-XRes]`, `out[i+XRes]` | `(pT>>1)+(pB>>1)` |
| odd_x, odd_y | diagonal | 4 corners | `(pTL>>2)+(pTR>>2)+(pBL>>2)+(pBR>>2)` |

User reports: **"the robot looks cartoonish AND the floor looks blocky especially on far
pixels"**.

Likely contributing factors (descriptive, not prescriptive):

- The matID similarity test is on the per-pixel material from the **G-buffer**, not surface
  normals or depth. Two pixels can share matID but live on different faces with very
  different `nGeo` (e.g. opposite walls of a hex column). With 3 of 4 pixels reconstructed by
  averaging colors from same-matID neighbors that have very different lighting, depth-
  discontinuous and orientation-discontinuous boundaries get smeared. On the floor at far
  pixels the same matID covers a large area with steeply receding depth → blocky 2×2 patches
  of identical lit color.
- The fallback runs the **scalar wave-1 kernel** (L2841+), not the vec body, even when
  OuterVec is on. So a quarter wave-2 fallback pixel on greets pays the full scalar shading
  cost. The fallback also re-samples the normal map, ambient, etc — divergent code in a
  tight tile.
- The 4-corner diagonal blend has a 2-bit precision loss per channel.
- TileFill always runs scalar, both waves.

Memory tag `project_strict_fill_test_no_payoff` notes that adding normal/depth checks to
the fill pass cost ~2.5 ms with no visible quality gain; that route is parked.

## 4. Cube shadow tap — `CubeShadow_Sample` at `FILLERS/ShadowMap.h:210`

> **STALE (2026-05) — mechanism only, and even some of that has moved.** The
> four separate `sm.polyId` / `sm.depth` (+`_dynamic`) planes below are now ONE
> interleaved `packSD`/`packDyn` word per texel; the line numbers are wrong; and
> the tap has since grown an 8x8 uniformity pyramid that resolves 80.4 % of taps
> without reading a texel at all. For the current shape and its costs read
> §00a's 2026-08-16l and 16m amendments: 16l splits the tap into a vectorised
> prologue (`--deferred_cube_prepass`) plus a scalar `CubeShadow_Tail`, and 16m
> makes `CubeShadow_Sample` a zero-frame leaf.

Steps per call:

| Step | Lines | Notes |
|---|---|---|
| Cube face select | L222 → `CubeShadow_SelectFace` L179-187 | dominant-axis on `worldP - lightWorldP`; 6 ops |
| ShadowMap fetch | L223 | one index `g_cubeShadowRefs[cubeIdx].faceIdx[face]` → `g_shadowMaps[..]` |
| `viewToLight` 3×3 matmul | L225-230 | 9 muls + 6 adds + 3 adds — produces `(lx,ly,lz)` in face view space |
| Near-plane reject | L236 | `lz <= 0.05f → return 1` |
| Face-frustum reject | L245-246 | `|lx|/lz, |ly|/lz` ≤ 1.5 |
| Projection | L247-249 | `smX = cntrX + perspX*lx/lz` and `smY` |
| NaN/sanity guard | L256-275 | `std::abort()` on insanely-out-of-range smX/smY; expensive only if it ever fires |
| Integer trunc | L276-277 | iX, iY |
| In-bounds reject | L278 | iX/iY against `sm.xres-1`, `sm.yres-1` |
| Bilinear weights | L280-285 | fx, fy, w00..w11 |
| **4 polyId taps from static buffer** (`sm.polyId`) | L306-307, 327-330 | 8 bytes (4×u16) |
| **4 polyId taps from dynamic buffer** (`sm.polyId_dynamic`) | L308-309, 336+ | 8 bytes |
| **4 depth taps from static** (`sm.depth`) | L310-311, 331-334 | 8 bytes |
| **4 depth taps from dynamic** (`sm.depth_dynamic`) | L312-313, 336+ | 8 bytes |
| `closestPoly` decision per tap | L316-324 (polyId), L364-367 (depth) | picks polyId of whichever buffer has the bigger occluder Z |
| Occlusion accumulate | L343-346 (polyId) or L368-371 (depth) | 4 cmps + 4 adds |
| Final blend | L373 | `return (occ >= 1) ? 0 : 1 - occ` |

User-noted measurement: ~32 ms total cube-tap time / ~1.44 M taps ≈ **22 ns / tap**, on
greets.

Cache footprint per tap (polyId mode): 4 × u16 polyId + 4 × u16 depth × 2 buffers = 32 bytes
of address space per buffer, but those 4 taps are at iX/iX+1 on rows iY/iY+1 — so 2 cache
lines per buffer × 4 buffers (2 polyId + 2 depth) = up to **8 cache lines per pixel per
omni**, with the four buffer streams (polyId static / polyId dynamic / depth static /
depth dynamic) for the same omni potentially hot at once. In depth mode the polyId loads
are skipped (only ~4 cache lines per pixel per omni).

Flags affecting it:

| Flag | Default | Effect |
|---|---|---|
| `shadow_polyid_no_pcf` | 0 | Single nearest-neighbor tap instead of 4 → ~9 ms saved on greets; jagged silhouettes |
| `shadow_polyid` | on | PolyId identity test instead of depth bias compare |
| `shadow_lightmap` | 0 | Skip cube tap on static mesh × static omni pixels; sample pre-baked atlas instead |
| `shadow_lightmap_planar` | 0 | Bake/sample atlas in world-axis-aligned planar projection |
| `shadow_lightmap_nearest` | 0 | Atlas: nearest instead of bilinear |
| `prof_no_cube_tap` | 0 | Short-circuit `resolveCubeAtten` → return 1.0 (full lit). The user's primary A/B handle. |
| `shadow_dynamic` | 0 | Each frame, re-bake animated meshes into `*_dynamic` buffers; lightmap path composites them on top via `dynamicOnly=true` cube tap |

## 5. Shadow render — `Render_DeferredShadowMaps` at `FDS/RENDER/Shadows.cpp:90`

The bake side. Called per frame from `GREETS.CPP:1850` (and 1856 for `shadow_dynamic`).

Three modes (`ShadowBakeMode`):

| Mode | Touches | Buffer |
|---|---|---|
| `StaticOnce` | static omnis (Omni_StaticShadow) | static `sm.depth`/`sm.polyId` |
| `DynamicOmnisPerFrame` | non-static omnis (mech-attached etc) | static `sm.depth`/`sm.polyId` (full rebake each frame) |
| `DynamicMeshesPerFrame` | static omnis, but **only animated meshes** projected; gated by per-mesh filter inside `Transform_Objects` flipped by `g_inDynamicShadowBake` | parallel `sm.depth_dynamic`/`sm.polyId_dynamic` |

Greets per-frame: `DynamicOmnisPerFrame` always (1850), `DynamicMeshesPerFrame` only when
`--shadow-dynamic` (1856).

`StaticOnce` runs once per scene init from `Initialize_Greets` → `ShadowMaps_BakeStatic` →
`FILLERS/ShadowMap.cpp:442`. Must happen **after** `Animate_Objects` so FLD-loaded omni IPos
splines resolve — see memory tag `project_static_bake_before_animate`.

Inside `Render_DeferredShadowMaps`:

| Phase | Lines | What | Threading |
|---|---|---|---|
| A: setup + Transform_Objects per light | 150-330 | Build per-light camera, swap `lightCtx`, clear depth+polyId buffers, enqueue `Transform_Objects` task per matching light | N tasks (1 per active shadow map) in threadpool |
| Barrier | 322-325 | Wait on `tileCounter` | |
| B: tile rasterize (light × tile) | 332-489 | Flat 6×4 tile job per light. Each task runs `clipper.Render(F, MekaleleShadowDepth, ...)` for each face that passed the mesh-level cull | N×24 tasks (flat) |
| Barrier | 490-494 | Wait | |
| Per-frame precompute `viewToLight` + `viewToLightOffset` | 549-565 | One affine per shadow map, used per pixel by the lighting kernel | single |

The flat (light × tile) batch in Phase B is intentional — straggler lights don't hold up
others. Mesh-level culling fires in `Transform_Objects` because:
- `g_currentShadowOmni` carries the omni pose so `sphereOutsideSpotCone` can reject meshes
  outside the cone.
- `g_currentShadowMap` carries `cubeFace` so the per-cube-face cull can pick the right face
  axis (gated on `--shadow-cube-face-cull`, default **on**).

The `Piramid` (greets wall mesh) chunking is set at scene init: one giant TriMesh becomes
N³ smaller TriMeshes via `--greets-piramid-chunk-grid=8` (default 8 → 512 cells, ~50-150
non-empty in practice). Without it the bsphere-vs-pyramid cull would never fire on the wall.

A `MatShadowCache` (L393-419) skips materials that look emissive (Mat_Transparent /
Mat_Additive / Mat_SkipZ or name-substring "lamp"/"emit*"). Lamp omnis don't self-occlude.

The shadow rasterizer is `MekaleleShadowDepth` (FILLERS/Mekalele.cpp / ShadowMap.h:93)
— depth-only (no color, no G-buffer, no texture). It's the rasterizer-side counterpart to
the deferred kernel's bilinear-PCF sampler.

`shadow_prof` (default off) prints per-frame Transform vs Raster cost rolling avg every
`FDS_SHADOW_PROF_INTERVAL` frames (60 default).

## 6. FeatureFlags inventory (perf-relevant subset)

All flags defined in `FDS/Base/FeatureFlags.def`. Forms: `--<name>`, `--no-<name>`,
`--<name>=value`, plus env `FDS_<NAME>`.

### Deferred

| Flag | Default | What | Note |
|---|---|---|---|
| `deferred` | FDS_DEFERRED_DEFAULT_ON | Master enable for deferred pipeline | |
| `deferred_zcull` | 1 | Cull G-buffer ROP writes against existing Z | |
| `deferred_vec` | 0 | 8-wide SIMD inner omni loop in scalar tile | Slower on arm64-via-simde |
| `deferred_outer_vec` | 0 (else Scene::PreferOuterVec) | 8 px × 1 omni outer kernel | Greets defaults off |
| `deferred_checkerboard` | 0 | Half-rate; see §3 | Works acceptably per user |
| `deferred_quarter` | 0 | Quarter-rate; see §3 | Robot cartoonish + floor blocky per user |
| `deferred_no_spec` | 0 | Skip specular | |
| `deferred_unified_tbr` | 0 | Unified transparent + particle TBR | |
| `deferred_gloss_stats` | 0 | Dump per-scene glossiness histogram | |
| `deferred_tile_stats` | 0 | Dump per-tile lighting cost stats | |
| `deferred_max_range` | 0.0 | Optional Range cap (cull only) | |

### Shadows

| Flag | Default | What |
|---|---|---|
| `shadows` | FDS_SHADOWS_DEFAULT_ON | Master |
| `shadow_polyid` | FDS_SHADOW_POLYID_DEFAULT_ON (on) | PolyId identity test vs depth compare |
| `shadow_polyid_no_pcf` | 0 | 1-tap nearest neighbor instead of 4-tap bilinear PCF |
| `shadow_backface_cull` | 0 | Cull back faces in shadow raster |
| `shadow_validate` | 0 | Capture clipper outputs outside input hull |
| `shadow_prof` | 0 | Per-light timing |
| `shadow_prof_cache` | 0 | Per-frame cache-line transition stats |
| `shadow_cone_cull` | 0 | Mesh bsphere vs spot cone cull |
| `shadow_skip_animated` | 0 | Static bake skips animated meshes |
| `shadow_dynamic` | 0 | Per-frame animated-mesh dynamic bake |
| `shadow_fzp_mult` | 3.0 | Shadow cam FZP multiplier over light IRange |
| `shadow_bias` | 512 | Constant Z bias |
| `shadow_slope_bias` | 1024 | Slope-scale Z bias |
| `shadow_lightmap` | 0 | Pre-baked per-face atlas; lighting reads atlas |
| `shadow_lightmap_res` | 16 | Atlas N×N per face |
| `shadow_lightmap_viz` | 0 | 1..9 different debug viz modes |
| `shadow_lightmap_recompute_bake` | 0 | Debug: skip atlas; call bake sampler per pixel |
| `shadow_lightmap_recompute_at_bary` | 0 | Debug: same but at runtime-stored bary |
| `shadow_lightmap_nearest` | 0 | Atlas nearest sample |
| `shadow_lightmap_planar` | 0 | World-axis-aligned planar projection bake/sample |
| `shadow_cube_face_cull` | 1 | Per-cube-face bsphere cull in Transform_Objects |
| `shadow_cube_vert_cull` | 0 | Per-vertex pyramid cull; only worth it for one giant moving mesh |

### Prof (diagnostic ablation gates)

| Flag | Default | What |
|---|---|---|
| `prof_no_tex` | 0 | Skip texture sample (uses 128/128/128) |
| `prof_no_lights` | 0 | Skip omni loop |
| `prof_no_spec` | 0 | Skip specular term |
| `prof_no_fog` | 0 | Skip fog pass |
| `prof_no_cube_tap` | 0 | `resolveCubeAtten` returns 1.0 |

These are the user's A/B handles; delta vs default = the gated stage's cost.

### Greets-specific

| Flag | Default | What |
|---|---|---|
| `greets_spot_height` | 0 | Override robot spotlight height (0 = scene default 4) |
| `no_greets_spots` | 0 | Skip code-installed robot + orbit spots |
| `greets_omni_shadows` | 0 | Mark all FLD omnis as `Omni_CastsShadow`+`Omni_StaticShadow` |
| `greets_omni_shadow_res` | 256 | Per-face cube map res for static FLD omnis |
| `greets_moving_omni_shadow_res` | 0 (=use above) | Per-face for moving omnis |
| `greets_piramid_chunk_grid` | 8 | N³ chunking of wall mesh; 8 → 512 cells |
| `greets_omni_default_range` | 1500 | Fallback IRange for FLD omnis with empty Range spline |

### Atmospherics (relevant because greets runs cones/halos by default)

| Flag | Default | What |
|---|---|---|
| `draw_cones` | 0 | Volumetric spotlight cones |
| `omni_halo_strength` | 0.0 | Halo brightness scale |
| `volumetric_unified` | 0 | Beer-Lambert unified pass |
| `vol_n_samples` | 4 | Ray-march samples |
| `vol_vec` | 1 | 8-wide SIMD per-sample inner loop |
| `vol_rect_cull` | 1 | Screen-rect cull per batch |
| `vol_halo_analytic` | 1 | Closed-form arctan integral |
| `vol_cone_analytic` | 1 | Cone analytic integral |
| `vol_prof` | 0 | Per-frame volumetric timing |
| `deferred_skybox` | 0 | Skybox-from-G-buffer pass |
| `fog_sigma_mult` | 3.0 | Beer-Lambert sigma |

### Display

| Flag | Default | What |
|---|---|---|
| `no_vsync` | 0 | Lets `SDL_RenderPresent` return immediately |

## 7. SIMD coverage map

| Operation | Path | SIMD | Width |
|---|---|---|---|
| G-buffer rasterize (Mekalele tile) | `FILLERS/Mekalele.h` | yes | 8-wide simde/AVX2-NEON; pixel-major SoA tiles |
| Mekalele octant normal encode | `oct_encode_u16_x8` (Mekalele.h:98) | yes | 8 |
| Mekalele octant normal decode | scalar in fallback, vec inline at L463-491 | partial | 8 in vec path; scalar in some leaf cases (`oct_encode_vec` memory tag) |
| Volumetric cones/halos per-sample loop | `Render_VolumetricCones_Tile` | yes (via `vol_vec`) | 8-wide pixel-major |
| Deferred fog pass | `Render_DeferredFogPass_Tile` L3328 | yes | 8 |
| Deferred lighting — scalar tile outer | `Render_DeferredLighting_Tile` | no | 1 |
| Deferred lighting — scalar tile inner omni loop | L1480+ | no | 1 |
| Deferred lighting — `deferred_vec` inner omni loop | L1267-1399 inside scalar tile | yes | 8 omnis × 1 pixel; off by default |
| Deferred lighting — OuterVec body | `_Tile_OuterVec` L2193+ | yes | 8 pixels × 1 omni |
| Deferred lighting — OuterVec normal-map TBN | L2400-2457 | no | per-lane scalar |
| Deferred lighting — OuterVec spec/water lanes | L2612-2694 | no | per-lane scalar fallback (no shadow taps) |
| Spec `pow(N·H, gloss)` | `pow_glossClass` (scalar tile) → template ladder of vec spec loops | yes for `{4,8,16,32,48,64,128}` | 8 omnis at a time |
| Cube shadow tap | scalar | no | 1 |
| 2D shadow tap (depth + polyId) | scalar | no | 1 |
| Atlas lightmap sample | `StaticShadowLightmap::sampleBilinear*` | no | 1 |
| Tile light list build | `buildTileLightLists` | partial | sphere-vs-tile cull is scalar |
| Shadow rasterizer | `MekaleleShadowDepth` (depth-only Mekalele) | yes | 8 — same pixel-major SoA |

## 8. Known issues, bugs, quirks

### Open

- **Shadow viz flicker even when paused** (task #83 created today). Mode is independent of
  scene tick, suggesting either the shadow-map dispatch is producing different output across
  frames despite identical scene state, or the overlay is reading from a buffer that's still
  being written by Phase B. `Render_DeferredShadowMaps` writes `g_shadowMaps[i].depth/polyId`
  with no read-side fence other than the Phase B barrier, but the V_Flip overlay
  (`ShadowMap_Overlay`) is also single-thread on the main thread after Render() returns.
  Still: needs investigation. Memory tag for follow-up if found: not yet created.
- **OuterVec kernel does not run 2D spot shadow attenuation** — the vec body in L2510-2566
  has no shadow tap, and the scalar fallback L2612-2694 also skips it. Cube shadows are
  similarly absent in both OuterVec paths. This is consistent with the description in
  memory tag `project_outervec_incorrect` (matches std visually only because greets uses
  cube shadows on static surfaces baked into the lightmap atlas, and most of the greets
  geometry actually goes through the scalar fallback anyway because of spec/nmap).
- **`std::abort()` in CubeShadow_Sample** (L274). If the smX/smY sanity guard ever fails the
  process dies. Per memory tag `feedback_no_defensive_backstops` this is intentional; called
  out here because it's not a typical fallback.

### Resolved / parked

- `project_outervec_incorrect` — OuterVec ~2 ms slower on greets (25 vs 23 ms); off by default.
- `project_strict_fill_test_no_payoff` — normal+depth checks on TileFill cost ~2.5 ms with
  no visible quality gain; reverted.
- `feedback_clipper_bary_lightmap` — Mekalele clipper invalidates per-pixel bary for atlas
  lookups when triangle is subdivided; `--shadow-lightmap-no-clipped` (default on) gates.
- `project_volumetric_simd_no_payoff` — pixel-major SIMD across rays wins ~21% at any N; on by default.
- `project_halo_analytic` — analytic atan integral replaces N-sample ray-march; on by default.

### Source-comment markers

`grep -rn 'TODO\|FIXME\|XXX'` in DeferredLighting.cpp + Shadows.cpp + ShadowMap.h returned
no hits — comments are mostly explanatory paragraphs, not deferred work markers.

### Quirks to keep in mind

- `Render_DeferredLighting` calls `getenv`-backed `FeatureFlags::xxx()` per pixel in
  multiple places. The accessors are cached-on-first-access, but per memory tag
  `feedback_use_feature_flags_for_hot_toggles` adding new hot-loop env reads must use
  FeatureFlags registry, not naked `getenv`. The kernel hoists every flag it touches to
  the top of `_Tile` (L963-994), but `resolveCubeAtten` re-reads several flags
  (`prof_no_cube_tap`, `shadow_lightmap_recompute_bake`, `shadow_lightmap_recompute_at_bary`,
  `shadow_lightmap_planar`, `shadow_lightmap_nearest`, `shadow_dynamic`) **per omni per
  pixel** when cube taps are reached. The FeatureFlags::xxx() calls are cached loads but
  not free, and the `prof_no_cube_tap` short-circuit is rechecked every tap. Same for the
  `g_shadowMode.load()` atomic in `CubeShadow_Sample` and `resolveCubeAtten`.
- TileFill always runs scalar even when wave 1 was OuterVec. Quarter-mode wave-2 cost is
  always paid in scalar.
- `Mat->Glossiness` not in `{4,8,16,32,48,64,128}` falls through to scalar libm `std::pow`
  in the `deferred_vec=1` path's default arm (L1452-1466). Should be impossible for shipping
  scenes (gated by `deferred_gloss_stats`).
- The greets per-frame call order is hard-coded in `GREETS.CPP::Tick_Greets`: Animate →
  greets-driver → Transform_Objects → Lighting → Radix_Sort → `gg->Render()` →
  `Render_DeferredShadowMaps(_, DynamicOmnisPerFrame)` → optional DynamicMeshesPerFrame.
  The shadow bake runs **after** the main Render() call, which means the lighting kernel
  inside Render() sees **last frame's** shadow maps. This is intentional (shadows trail by
  one frame to allow async on the next frame), but is worth flagging if assuming "this
  frame's bake feeds this frame's lighting".

  WAIT — re-reading `RENDER.CPP::renderFrame` and the Greets driver: `gg->Render()` is
  `fds::RenderPipeline::renderFrame` (RENDER.CPP:279). It contains `Render_DeferredLighting()`
  at line 371. So lighting fires *inside* Render(), then Greets calls
  `Render_DeferredShadowMaps` *after*. The first frame's lighting reads zero-initialized
  shadow maps; subsequent frames read N-1's bake. Confirmed by code; flagged as a perf
  surprise.

## 9. Measured numbers — today's snapshot

Greets `t=500`, 1920×1080, low-poly Piramid (5.5k faces), chunks on, cube-face cull on.

| Config | mean ms |
|---|--:|
| No shadows | 33 |
| Dynamic only (per-frame mech omni rebake; `--shadow-dynamic` off) | 90 |
| Lightmap only (`--shadow-lightmap` on; static omnis via atlas) | 73 |
| Both (lightmap + dynamic) | 89 |

Component decomposition:

| Component | ms |
|---|--:|
| Cube tap (delta from `--prof-no-cube-tap` at full shadows) | ~32 |
| Shadow render (`Render_DeferredShadowMaps` Phase A + B) | ~12.5 |
| Kernel "other" (everything in `_Tile` except cube tap + shadow render) | ~12 |
| Baseline-no-shadow (kernel + raster + fog + volumetric + flip etc) | 33 |

960×540 (quarter pixel count): ~33.6 ms.
- Frame floor (non-pixel-bound work + vsync alignment): ~16 ms.
- Pixel-bound work at 540p: ~17.6 ms.
- Linear extrapolation to 1080p: ~70 ms pixel-bound, matches the "both" measurement once
  shadow render is amortized.

Pixel-bound cost is therefore the dominant scaling factor. Resolution halving on each axis
divides the kernel + cube tap cost by ~4 directly.

Cube tap rate: 1920×1080 × ~5 omnis-per-pixel × ~0.15 hit-rate (after tile cull) → ~1.44 M
taps/frame at 32 ms → ~22 ns/tap. This includes the bilinear PCF + closestPoly + bounds
checks; the four cache-line read pattern at 64B/line × 4 lines × ~5 ns/L2-hit accounts for
most of it.

---

## 10. Flicker investigation

Symptom: with greets paused, the shadow viz overlay (B/V keys) shows small patches that
change frame to frame. Inputs (camera, omni positions, geometry, ShadowMatIDs) are constant
across paused frames, so the only way the overlay color can change is if the shadow buffer
(`sm.polyId` / `sm.polyId_dynamic`) itself changes — i.e., Render_DeferredShadowMaps
produces non-deterministic output for identical inputs. Candidates below are ordered by how
directly they explain "polyId differs frame-to-frame at fixed inputs."

### Candidate A — Shared tile-boundary pixels racing in `ShadowBarry::apply_exact` (HIGH)

**Where:** `FDS/FILLERS/ShadowMap.cpp:482-551` (`apply_exact`), dispatched from
`FDS/RENDER/Shadows.cpp:368-489` (Phase B tile loop).

**Why it's a candidate:** Phase B enqueues 24 tile lambdas per shadow map onto the shared
`ThreadPool`. The lambdas for one shadow map all read the same per-light `FaceListContext`
and all write to the same `sm.depth` + `sm.polyId` buffers. Each lambda's clipper limits its
output to its tile's `(x1f, y1f, x2f, y2f)` rect.

The shadow-pass tile rect is `tileSizeX = (sm.xres + 5)/6`. At sm.xres=512 that's 86 — not a
multiple of `TILE_SIZE=8`. So the clipper's per-tile rect boundaries fall *inside* an 8×8
ShadowBarry tile. Two adjacent tile-workers (e.g. tx=0 covering x∈[0,86) and tx=1 covering
x∈[86,172)) can both produce clipped polygons whose ShadowBarry tile loop touches the
global 8×8 tile at x∈[80,88) — worker 0 wants to write pixels 80-85, worker 1 wants 86-87.

`apply_exact` at line 530 does `*(__m128i*)zRow = _mm_blendv_epi8(*(__m128i*)zRow, encU16,
maskU16)` — a 16-byte read-modify-write of the whole 8-uint16 row chunk. This is *not*
atomic. If worker 0 reads the 16-byte word, worker 1 reads it concurrently, both blend
their own lanes, and both write back, one worker's lanes are silently lost. Same chunk also
covers the polyId scalar loop at lines 535-540, but those are independent 1-uint16 stores
per lane, so polyId per-pixel race is only an issue when *the same* pixel is masked-in by
*both* workers (i.e., a vertex landed exactly on the shared edge after clipper rounding,
which `lroundf(PX * 16)` will produce often when both triangles' edge passes through the
same integer subpixel coord).

When two coplanar triangles with *different* `ShadowMatID` (greets wall split assigns one
per cluster) share an edge that the clipper cuts across the boundary, the shared-edge
texel's polyId is whichever worker wrote last. With pool scheduling non-deterministic, the
winner flips frame-to-frame even at fixed inputs → patches at cluster seams.

This also explains why city hasn't shown the same flicker: city has fewer `ShadowMatID`
distinctions (no wall-split), so the polyId of the two contenders is usually the same and
the race is invisible.

**How to verify:**
1. Pause greets, B-key the overlay on, then run with `--shadow-tiles=1` (or any flag that
   forces serial Phase B tile execution) and see if flicker disappears. If yes → race.
2. Or: after `Render_DeferredShadowMaps` returns, FNV-1a hash `sm.polyId` for one map and
   log it. If two consecutive paused frames produce different hashes, polyId is being
   written non-deterministically — already confirmed by the `[SHADOW-STALE]` machinery in
   `ShadowMap.cpp:60-98`, just run it under `T` key while paused.
3. Sanity check: change `tileSizeX/Y` to be rounded UP to a multiple of `TILE_SIZE` (8) so
   the per-tile clip rects align with ShadowBarry's 8×8 grid. Flicker should disappear.

**Likelihood:** HIGH. Symptom (patches flicker on pause) matches exactly: race winner
varies with pool scheduling, identical inputs aren't enough to fix the output.

### Candidate B — `tile.x * TILE_SIZE` offset is global, but tiles aren't pixel-aligned (HIGH, same root as A)

**Where:** `FDS/FILLERS/ShadowMap.cpp:483-489`.

```cpp
uint16_t * const zRowBase  = zArr
    + size_t(tile.y) * barry::TILE_SIZE * size_t(xres)
    + size_t(tile.x) * barry::TILE_SIZE;
```

`tile.x` is in 8-pixel units of the *global* shadow map. So workers from different clipper
rects writing to the same global `tile.x` are aliasing the exact same memory region. This
is the second face of (A) — the writes don't just go to nearby memory, they go to *identical*
16-byte words.

Combined with the non-8-aligned clipper rects, two workers WILL touch the same word at
seams.

**How to verify:** Print `(tile.x, tile.y, worker_thread_id)` for every `apply_exact` call
for one shadow map's frame and check for duplicates. Any (tile.x, tile.y) pair from two
different workers proves the aliasing.

**Likelihood:** HIGH. Same root cause as A.

### Candidate C — `MekaleleShadowDepth` skipMipLevel + clipper bary edge case (MEDIUM)

**Where:** `FDS/FILLERS/ShadowMap.cpp:866-907` (clipper validation block), `Shadows.cpp:466`
calls `clipper.Render(..., skipMipLevel=true)`.

**Why it's a candidate:** The `--shadow-validate` block logs clipper outputs whose verts
escape the input triangle's convex hull. There's a memory note about [Mekalele clipper
invalidates per-pixel bary for lightmap lookups](feedback_clipper_bary_lightmap.md) — if a
similar bary-mismatch path is firing for the shadow rasterizer (per-pixel polyId stamp
written based on a face whose bary doesn't actually cover the texel), the polyId at edge
texels could be the *wrong* face's ID, which then races with the correct face's per-tile
write. That makes the patches more visible (correct vs wrong cluster) instead of just
"different cluster on each tie."

But the bary itself is constant across frames for fixed geometry — so this alone wouldn't
explain "different frame to frame." It only amplifies the racing in A/B.

**How to verify:** Run with `--shadow-validate`. If `[SHADOW-CLIP]` lines spam during
paused playback for the same tile each frame, the clipper is producing extra verts; if they
fire only intermittently, it's interacting with the race.

**Likelihood:** MEDIUM. Plausible amplifier of A/B but not the root cause of the time-
varying behavior.

### Candidate D — Stale `g_cubeShadowRefs[*].lightISource` mismatch with `sm.lightISource` (MEDIUM/LOW)

**Where:** `FDS/RENDER/Shadows.cpp:101-103` updates `cr.lightISource = cr.omni->IPos` at the
top of every shadow-render call. `sm.lightISource = O->IPos` is updated again per-light in
Phase A loop at line 249. These are two separate copies and both must agree for
`CubeShadow_Sample` to return correct values (lines 219-223 of `ShadowMap.h`).

The update at line 101-103 iterates `g_cubeShadowRefs` AT ENTRY, before checking the mode
or the omni's active flag. So even if Render_DeferredShadowMaps does nothing for this omni
this frame (wrong mode), cr.lightISource still gets refreshed from the current IPos. Phase
A updates `sm.lightISource` only for omnis included in the mode filter. If the omni is
active for one mode call (DynamicOmnis) but not the other (DynamicMeshes), the two
buffers are in sync after the first call but Phase A's lightISource update from the second
call doesn't run for this omni → still in sync. So this isn't a bug source.

**Why low:** This wouldn't be time-varying under pause (IPos is constant when paused).

**How to verify:** Skip — not a likely flicker source.

**Likelihood:** LOW.

### Candidate E — `thread_local` capture-by-pointer of main thread's `perLightFaces` etc. (LOW)

**Where:** `FDS/RENDER/Shadows.cpp:139-142` declares `static thread_local
std::vector<fds::FaceListContext> perLightFaces;` and friends. Lambdas at lines 300-319
and 376-486 capture pointers (`facesPtr`, `scratchPtr`, `camPtr`) into these vectors.

**Why it's a candidate:** thread_locals live in the thread that called the function. The
main render thread calls `Render_DeferredShadowMaps`, so `perLightFaces` lives on the main
thread. Worker threads dereference pointers that point into another thread's TLS.

The C++ memory model says this is OK as long as the source thread doesn't destroy the
storage before workers finish reading. The barrier between Phase A and Phase B + the final
Phase B barrier guarantee this. Lifetimes are also fine — `static thread_local` lasts the
whole program. So no UAF.

But: if a second concurrent caller existed (e.g. a debug path that called
Render_DeferredShadowMaps from another thread), they would have independent TLS copies and
weird things would happen. As of the current code path, only the main thread calls it, so
no race here.

**How to verify:** Skip — not a flicker source as written.

**Likelihood:** LOW (latent risk only).

### Candidate F — `ShadowMap_TickStalenessTracker` reads `polyId_dynamic` as `uint8_t` (LOW, diagnostic only)

**Where:** `FDS/FILLERS/ShadowMap.cpp:75`: `for (uint8_t v : sm.polyId_dynamic)`. The vector
is `vector<uint16_t>` (header line 49). Iterating with `uint8_t v` truncates each entry.

**Why low:** This is a hash computed for the diagnostic `[SHADOW-STALE]` print only. It
doesn't write to `polyId_dynamic` and doesn't affect rendering. But it does silently lose
the upper byte of each polyId, so the stale-tracker will sometimes report "unchanged" when
only the high byte changed → understates actual non-determinism. Worth fixing alongside
because it'd mask future bug repros.

**Likelihood:** LOW for visible flicker, BUT it would hide regressions in the staleness
tracker.

### Candidate G — `sm.fzp` / `sm.zScale` set at scene init but `fzp` used by Phase A clear / Render (LOW)

**Where:** `ShadowMap.cpp:371,419` set `sm.fzp = O->IRange * sFzpMult` and `sm.zScale =
0xFF00 / (sm.fzp * 1.1f)` at Rebuild. `Shadows.cpp:254` overwrites `sm.zScale` per frame.

**Why low:** Per-frame overwrite is deterministic given constant inputs. Not a flicker
source unless `O->IRange` itself drifts when paused (it doesn't — IRange isn't on the
animation spline).

**Likelihood:** LOW.

### Most likely root causes

1. **Candidate A/B (same bug, two angles)**: ShadowBarry's per-tile `_mm_blendv_epi8`
   read-modify-write on global 8×8-pixel words combined with non-8-aligned Phase B tile
   rects (`tileSizeX = (sm.xres + 5)/6 = 86 @ 512²`) lets two tile-workers concurrently
   stamp the same texel with different ShadowMatIDs. Race winner varies with pool
   scheduling → patches flicker frame-to-frame even when inputs are frozen.
2. **Candidate C** (clipper bary edge case) is a plausible amplifier — wrong-face polyId at
   the seam makes the visible delta larger when the race winner flips — but it's a
   secondary, not the cause of the time-varying behavior.

---

## Quick index

- Pipeline orchestration: `FDS/RENDER/RENDER.CPP::renderFrame` (L279), `GREETS.CPP::Tick_Greets` (L~1700)
- Deferred entry: `FDS/RENDER/DeferredLighting.cpp:3032`
- Scalar kernel: `DeferredLighting.cpp:953`
- OuterVec kernel: `DeferredLighting.cpp:2193`
- Wave-2 fill: `DeferredLighting.cpp:2728`
- Cube tap: `FDS/FILLERS/ShadowMap.h:210`
- Cube tap dispatch (lightmap vs cube): `DeferredLighting.cpp:813`
- Shadow render: `FDS/RENDER/Shadows.cpp:90`
- Per-pixel 2D shadow tap (scalar): `DeferredLighting.cpp:1521-1640`
- Dispatch logic OuterVec: `DeferredLighting.cpp:737`
- Sub-pixel dispatch: `DeferredLighting.cpp:752,767`
- Fog pass: `DeferredLighting.cpp:5411`
- FeatureFlags: `FDS/Base/FeatureFlags.def`
- Lightmap atlas sampler: `FDS/Base/StaticShadowLightmap.h:130-220`
