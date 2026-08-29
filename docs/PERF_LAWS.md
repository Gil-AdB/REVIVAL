# PERF_LAWS.md — the campaign's laws and traps, in one place

**What this is.** Between 2026-08-28 19:00 and 2026-08-29 08:45, four agents ran
~20 optimisation rounds against this renderer. The *findings* are in
`docs/PERF_STATE.md` (§00k…§00v) and `docs/OPTIMIZATION_BACKLOG.md`. What is
here instead is the part that outlives them: **the rules that kept turning out
to be true, and the traps that kept costing hours.** Each line carries its
evidence and the round that established it, so nothing here has to be taken on
authority.

**Read this before proposing an optimisation to this tree.** Most of the ideas
that feel obvious on arrival are already on the refuted list below, with
numbers.

---

## PART 1 — OPTIMISATION LAWS

### L1. A fan-out pays only above a work-per-dispatch threshold. *(4 sightings)*

Parallelising a pass is not free and is frequently negative. The threshold sits
somewhere around **tens of microseconds of work per dispatch**:

| sighting | work | result |
|---|---|---|
| city glass fan-out | 262 k px/dispatch | **−24 %** |
| RTT cone pass fan-out | ~64 µs total | **+139 %** (0.064 → 0.153 ms) |
| mirror-mask pool clear | bandwidth-bound `memset` | **+33 %** — kept default-OFF as its own proof |
| `--mirror_rtt_pool`, `--city_glass_pool` | large per-dispatch | **wins, landed by default** |

**Rule:** before fanning anything out, measure the pass. If it is under ~100 µs,
the round trip is the cost. `PERF_STATE.md:515`, `OPTIMIZATION_BACKLOG.md:1028`.

### L2. An 8-iteration independent scalar loop is not automatically improved by vectorising it. *(3 sightings)*

A short scalar loop whose iterations are independent is *already* extracting
ILP; the vector form serialises it behind a gather/pack. Sightings: the SSAO
march's depth gather (**bit-exact, −9.8 % instructions, +2.5 % cycles** —
refuted); the cone kernel's `FDS_CONE_W4` (**+7.5 % cyc, +8.5 % wall**); C6's
midpoint closed form (below). **Price it in cycles, never in instructions.**

### L3. Register pressure beats op count in the cone and lighting kernels.

**C6** (midpoint `W²`/`D·W` closed form) did exactly what its arithmetic
promised — `fmla.4s` −4, `fmul.4s` −2, `fsub.4s` −2, `fneg.4s` −2 = **−10
vector-ALU ops** — and lost anyway: `ldr` +15, `str` +14 = **+29 spills**,
net +1.41 % instructions, **+5.42 % cycles, +4.97 % wall**. On arm64 one
`__m256` is TWO of the 32 v-registers, so keeping six values live across a block
costs more than the block saves. Same lesson as B8's "the arrays are not a
buffer, they are a phi node". `OPTIMIZATION_BACKLOG.md` 2026-08-29 (C6).

### L4. Once a kernel is issue-bound, instruction counts stop predicting time.

The cone pass runs at **~81 % of this core's NEON issue ceiling**; there,
removing instructions removes time. Elsewhere it does not:

* the movemask sweep removed a real 12–25-instruction sequence at 8 sites —
  **cone kernel −4.4 % instr → −5.1 % WALL**, but **`lighting-w1` −2.0 % instr →
  +0.7…+1.4 % cycles**, IPC **4.08 → 3.94**. Those slots were not the
  constraint, and the replacement's latency matched what it replaced.
* `--shadow_polyid_no_pcf`: **−4.35 % instructions, −0.33 % cycles.**

**Rule:** quote cycles and wall for a claim; instructions only as the
deterministic column (they reproduce to ~0.1 %, cycles to ~5 %, wall to ~30 %).

### L5. `cores = Gcyc / clock / wall` finds both the wins and the refutations.

The cheapest triage in the tree. A row whose implied core count is far below the
worker count is serial or barrier-bound (a fan-out candidate); one at the worker
count is compute-bound (only fewer operations help).

### L6. The only lever with real headroom on a saturated per-pixel row is FEWER PAIRS.

Reached independently by two campaigns. The cone pass: "fewer (px × spot) pairs"
is the only lever its own carry-forward names, and `--cone_range_cull` took
**−52.6 %** of it by removing pairs. greets `lighting-w1`: 8.40 lights/px enter
and only 2.06–3.40 accumulate, at ~500 instructions per surviving pair, with no
attachable sub-feature above the noise. Micro-optimising the body is exhausted;
the remaining prize is in not entering it.

### L7. Bit-exactness is won by SHARING SOURCE, not by transcribing.

The froxel composite's water-reflection leg went 8-wide **bit-exact on the first
build, 28 warm frames across six arms**, because the leg was lifted verbatim
into one `static inline` helper that both paths call — no second spelling to
keep in agreement, nothing re-associated, and the §00k2 FMA-contraction rules
were never needed. When you *must* transcribe, those three rules decide it:
`a*b − c*d` contracts to one `fnmsub`; `A+B+C` of products chains from the
**second** term; a trailing `x*poly` feeding an add/sub is never materialised
alone. `PERF_STATE.md` §00k2.

### L8. Anything that runs per-pixel and is decidable per-tile or per-light is the first place to look — and the guard usually already exists.

Wins of this shape: the shadow-FList bbox pre-reject (−82 % entries), the
`computeMapShadowAtten` 3-way-AND guard, the cone-hull tile rect (−20…−39 % of
pairs), `ConeSpotPre`. **Check whether it is already done before proposing it** —
the round-12 census walked into exactly that trap and had to record it.

### L9. In a kernel at its register-allocation limit, a new runtime predicate costs more than the work it removes.

`OPTIMIZATION_BACKLOG.md:4783`, established over eleven greets rounds: a
never-taken dynamic-plane skip inside the cube tap cost **+12.4 %**; a runtime
bool hatch cost **+4.3 % with the flag OFF**. Corollary: **the cube tap only
gets cheaper by being CALLED less.** Compile-time constants are fine; live bools
are not.

---

## PART 2 — MEASUREMENT TRAPS

### T1. A green 13/13 does not mean a path was exercised. ★ the expensive one

Every pinned pose is a **one-tick `--snapshot`**, and several live, default-ON
paths only switch on from the **second** tick. Proved, not argued: a binary that
**does not compute city's water-reflection fog at all** passes **12/12 pins** and
fails **5 of 7 warm rows** — and in every failing row *frame 1 matches and only
ticks 2+ diverge*. Use `tools/warm_gate.sh` (fast pair always; `--full` is 7 rows
in **18 s**). Census of tick-1-cold paths: `OPTIMIZATION_BACKLOG.md` 2026-08-29c.

**T1b — the second-order cost: a cold census mis-prices the backlog.** The
froxel composite's scalar half was on file at **0.030 Gi/f / 0.72 %** of
`renderFrame`, measured cold at 1512×848. Warm at 1920×1080 it is **5.15 %** —
**7× low**. A cold census does not merely miss regressions; it aims the campaign
at the wrong row.

### T2. Stale shares get stale — re-take the measurement before sizing a plan off it.

`PERF_STATE` §2 carried "the cube tap is 36.6 % of the row" for eleven rounds.
Re-measured 2026-08-29: **ALL shadow work is 16–28 %**. The levers those rounds
landed did their job and nobody re-took the number, so every plan sized off it
aimed at roughly twice the prize that existed.

### T3. `TailProf::hwRead()` is `proc_pid_rusage(getpid())` — PROCESS-WIDE.

A nested scope inside a threaded pass measures **every thread**, not its own.
Threaded sub-scope splits must be done by **ablation differencing** (build with
the stage cut, difference the totals), never by nesting a timer.
`PERF_STATE.md:214`.

### T4. `ChdirToAssetRoot` makes a binary's FILE LOCATION select its assets.

`DEMO` chdirs to its **own** directory, not your shell's CWD, so a worktree
binary always renders that worktree's assets and writes its `cache/`.
`cd Runtime && /other/tree/DEMO` does not do what it reads like.
**Three separate false alarms traced to this.** Use `--no-chdir_assets` to gate
one tree's binary against another's assets. `OPTIMIZATION_BACKLOG.md:199`.

### T5. chase pins are POSE-SEQUENCE dependent.

t=800 rendered **alone** differs from t=800 rendered **third of five** by
**434 591 px (20.96 %), max |Δ| 5**. Only the first pose of a process is
comparable across recipes. Run the pinned recipe verbatim, including the pose
list. `PERF_STATE.md:590`.

### T6. `pgrep -f` matches your own shell.

Two agents' box-quiet wait loops deadlocked on each other, each seeing the
other's `pgrep` command line as a running `DEMO`. Use
`ps -Ao comm= | grep -c '/DEMO$'` (`scratchpad/quiet.sh`), which matches
executables, not argv.

### T7. `--ssao_dump` inflates the pass it measures by ~3.5×.

It forces the scalar apply loop (~4.2 ms → 13.7–16.0 ms). It is a correctness
instrument, never a perf one.

### T8. `cmake --install` fails on a pre-existing rpath step. That is NOT a failed build.

`install_name_tool: no LC_RPATH load command with path: /opt/homebrew/lib`.
The binary is placed by the build's own POST_BUILD copy; verify with
`--no-chdir_assets` against stock assets rather than trusting the install's exit
code.

### T9. A row that emitted no frames must not print as a pixel mismatch.

`warm_gate.sh` printed an empty `got` under a `FAIL` header when a row produced
nothing — which reads exactly like a pixel regression and sends the next reader
hunting one. **Cost an hour.** It now reports
`ERROR <row> ran but produced N/M frames (exit R)`. Generalise: an instrument
that cannot distinguish *broken* from *different* will manufacture ghosts.

### T10. 24 repetitions of a blind test is still a blind test.

The `bsWorld` regression passed 13/13 pins **and 24 consecutive runs** of the
city acceptance pose. All 24 were one-tick snapshots; the defect begins at
tick 4. Repetition buys confidence against *noise*, never against *blindness*.

### T11. A flag that gates half a change is not a revert arm.

Round 5's city glass change shipped a fan-out and a `bsWorld` cache under one
flag — and the flag did not gate the cache, which ran on the serial path too. If
`--no-<flag>` does not restore the parent binary byte-for-byte, it is not a
control.

### T12. A warm-gate red from someone else's change is not yours to re-baseline.

The `bsWorld` red presented as **one pixel, max |Δ| 1–2, deterministic 3/3**,
ticks 1–3 and all 13 pins clean, cleanly isolated to the merge — every fact true,
and every fact also what a stale cached bounding sphere looks like. It was
re-baselined for ten minutes before its owner found the bug. **Report it to the
owner; leave the baseline alone.** "Small and deterministic" is evidence about
magnitude when the question is mechanism.

### T13. Between-binary comparisons carry an LTO-layout floor.

**±0.9 % on `renderFrame`**, and layout alone has produced 6 % swings on a row.
Prefer a same-binary flag flip; when a compile-time A/B is unavoidable, diff the
per-function instruction counts to prove the change is localised (the movemask
sweep did: one function moved in a 674 776-instruction image).

### T14. Instruments need the frames they were built for.

Both shadow censuses report every **8 main-view frames**; a one-pose snapshot
renders one and prints nothing. The cone and fog censuses report the **previous**
frame. Use `--bench` or a multi-pose snapshot, and read the *second* report.

---

## PART 3 — WINDOW STATE AT PUBLICATION (2026-08-29 08:0x)

**`fog-wt` tip: `b2c9bf8c`** (this document's commit is its child).
**Gates on that tip, this worktree, stock `rev.cfg`, 1920×1080:**
13/13 pinned poses · `tools/render_gate.sh` **4/4** ·
`tools/warm_gate.sh --full` **7/7**.
**Installed `Runtime/DEMO` is that tip** and reproduces the `city-warm` warm
baseline exactly when pointed at stock assets with `--no-chdir_assets`
(`aa45e9a6… 2c56ce52… de6db0e3… cb4db07e… 540ae440…`).

**What Gil-Ad gets by launching the demo** (§00v, default-vs-default):
city t=1961 whole tick **−9.3 %**, `renderFrame` **−11.6 %**; chase t=800
`renderFrame` **−17.7 %**; greets t=5743 **−2.3 %**.

### The top three remaining opportunities, with measured sizes

1. **greets `lighting-w1` — the per-(8×8 block × light) cube-tap cull.
   2.3 ms (t=5743) to 5.3 ms (t=3409).** 70.8–90.9 % of cube taps sit in an 8×8
   screen block where every tap agrees, and at t=3409 the tap is called 2.58 M
   times to reject 1.07 %. **Requires** a hi-Z min/max pyramid over `ZPage16`
   (only per-TILE bounds exist today, and tile bounds over an 8×8 block give a
   frustum slab whose cube footprint is never provably uniform), a hierarchical
   cube-footprint query, and the pixel-major → block-major restructure §2 calls
   "large". Not micro-optimisation. `OPTIMIZATION_BACKLOG.md` 2026-08-29d.
2. **The transparent composite — `Render_DeferredTransparentLighting_Tile`,
   72.5 % of `TBR-render`'s interior, 0.626 Ginstr/f**, shading 1.578 M live
   transparent pixels at **~36 ns and ~400 instructions each** (§00u). The
   attribution closes to 99.8 %, so the row is now fully mapped and nobody has
   attacked its dominant child. Note §00t separately CLOSED city's `TBR-render`
   as irreducible by its own strip-bound lever (already 91.58 % live).
3. **The froxel composite's reflection leg, vectorised — ~0.12 Gi/f**, the residue
   after 2026-08-29b took 57 % of the punt's cost. **This one does need the
   §00k2 contraction rules**: `gY = w10*Xc + w11*Yc + w12` and
   `uV = Xc*Xc + Yc*Yc + 1` are exactly rule 2's "A+B+C of products" shape.

### Open at publication

**greets `lighting-w1` +1.8 % / `lighting-w2` +2.9 %** at t=5743 (dup-arm drift
±0.1 %, so real, not noise). greets runs the SCALAR wave-1 kernel that no round
touched; the leading hypothesis is code layout, since the window added a great
deal to `DeferredSurfaceKernel.cpp`. **A peer was still live on this at
publication time — see §00v and their round notes for the resolution.**
