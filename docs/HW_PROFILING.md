# HARDWARE-ASSISTED PROFILING

`--deferred_prof` says which stage is slow. This document is about **why** — and
about being honest, on this specific machine, regarding which of those "why"
questions are answerable at all.

Companion flag: `--hw_prof` (FeatureFlags.def, default OFF, byte-null). It adds
hardware-counter columns and `os_signpost` intervals to the phase boundaries
`--deferred_prof` already brackets, rather than defining its own — so the stage
names, the main/offscreen split and the warmup exclusion can never drift apart
from the wall-clock table.

---

## 1. What is actually reachable on this box

Measured 2026-08-10 on the M2 Max, macOS 26.5, SIP enabled, no sudo.

| capability | status | evidence |
|---|---|---|
| `xctrace` / Instruments templates (Time Profiler, CPU Counters, Allocations, System Trace, VM Tracker) | **REACHABLE since 2026-08-12** — see §1a | Xcode 26.6 is now installed. `xcrun xctrace` still fails, but that is a *selection* problem, not an availability one (§1a). |
| PMU event selection (L1D/L2 misses, stalls, branch mispredicts) via `kperf`/`kpc` | **UNREACHABLE without root** | `kperf.framework` and `kperfdata.framework` are present, and `sysctl kpc.pc_capture_supported` = 1 — but `sysctl -w kpc.counting=1` → **`Operation not permitted`**. Counter *configuration* is root-only. |
| `powermetrics` | root-only | needs sudo; **a user-run option**, not an agent-run one |
| `dtrace` | effectively unreachable | SIP enabled + root required |
| **per-task retired instructions + core cycles** | **REACHABLE, unprivileged** | `proc_pid_rusage(RUSAGE_INFO_V4+)` → `ri_instructions`, `ri_cycles` |
| **per-THREAD retired instructions + core cycles** | **REACHABLE, unprivileged** | `thread_selfcounts(1, buf, 16)` → `{instructions, cycles}` |
| energy | reachable | `rusage_info_v6.ri_energy_nj` |
| symbol-level time attribution | reachable | `/usr/bin/sample` |
| allocation audit | reachable | `MallocStackLogging` + `malloc_history`, `heap`, `vmmap`, `leaks`, `footprint`, `/usr/bin/time -l` |
| `os_signpost` readback without Instruments | reachable | `log show --signpost --predicate 'subsystem == "com.revival.fds"'` |

**The one thing you cannot have is a cache-miss rate.** There is no unprivileged
substitute on Apple silicon. §6 is about what to use instead, and what that
substitute can and cannot prove.

One loose end, recorded so nobody re-derives it from scratch: §1a is right that
the CPU Counters template does not lift the root-only PMU restriction *for what
this repo can read per phase*, but the template itself **does record
successfully unprivileged** (verified 2026-08-12, `rc=0` launching DEMO
headless), and its `.trace` carries a `pmc-events` array per sample plus a
topdown metric set whose own legend reads *Cycles / Instruction Delivery
Bottleneck / Discarded Bottleneck / Instruction Processing Bottleneck / Useful*.
**Nobody has decoded that array into named events** — 12 slots, the trailing
ones constant-looking, unmapped. So a stall-*class* breakdown may be reachable
through Instruments; a per-event miss rate is still unproven. Do not cite this
as a miss rate until someone decodes it.

### The counter backbone, and why it is trustworthy

Two properties had to be verified before building anything on `proc_pid_rusage`:

1. **They are real hardware counters, not estimates.** A 200 M-iteration FP loop
   read 1 401 052 034 instructions / 2 403 994 171 cycles from
   `proc_pid_rusage`, and `thread_selfcounts` returned 1 401 044 302 /
   2 403 981 305 for the same region — two independent kernel paths agreeing to
   **0.001 %**. (This also fixes the undocumented field order of
   `thread_selfcounts`: **buf[0] = instructions, buf[1] = cycles**.)
2. **They are LIVE for threads still on-core**, not just flushed at context
   switch — otherwise bracketing a parallel wave would read zero. A mid-flight
   read of a task with 4 spinning threads over a 0.5 s window returned 1.89
   core-seconds of cycles where ~2.0 was expected. Worker activity *is* captured.

Cost, measured with 12 threads spinning: `proc_pid_rusage` **808 ns/call**,
`thread_selfcounts` **256 ns/call**. At ~40 phase boundaries × 2 reads that is
**0.065 ms/frame** — 0.07 % of a 90 ms greets frame. With the flag off, zero reads.

---

## 2. The flag

```sh
--deferred_prof=1 --hw_prof
```

`--hw_prof` alone does nothing: it appends three columns to the `--deferred_prof`
table.

| column | meaning |
|---|---|
| `Ginstr/f` | retired instructions per main-view frame, billions, **task-wide** |
| `Gcyc/f` | core cycles per main-view frame, billions, **task-wide** |
| `IPC` | `Ginstr / Gcyc` |

Task-wide means *summed over every thread*, so a parallel wave's counters cover
all 12 workers — the same accounting as the existing `thrsum` column, and unlike
`wall`. It also means a phase measured while an unrelated bake runs on a pool
worker will over-count; `calls/f` and `effPar` are what expose that.

Phases booked through `mark()` / `addMs()` (`light-list`, `depth-bounds`,
`mirror-grid`, `tile-cull`, `strip-lists`, the `rf-*` prologue marks, the
`xpar-*` splits) have no counter stamp to difference against and print `-`
rather than a wrong number.

---

## 3. Recipe (a) — per-stage time + counters

```sh
cd Runtime
../scratchpad/hwprof_stage.sh greets 5743 20
../scratchpad/hwprof_stage.sh city   1961 20
```

Emits the `[DPROF]` table with the counter columns, then a `sample(1)` symbol
profile of the same workload (launch → settle past init and the lazy first-frame
bakes → sample the steady state). Cross-read them: a symbol that is hot in
`sample` **and** sits in a low-IPC phase is the memory-bound one.

## 4. Recipe (b) — allocation audit

```sh
cd Runtime
../scratchpad/hwprof_alloc.sh greets 5743 14
```

`/usr/bin/time -l` for peak RSS and fault counts over a whole run, then a live
snapshot under `MallocStackLogging` — `footprint`, `vmmap -summary`, `heap`,
`malloc_history -allBySize`, `leaks`. This **complements `--mem_census`**, it
does not replace it: the census knows what each engine buffer *is* and which
variable its size scales with; this knows only bytes and call stacks, but it
sees allocations nobody has taught the census about. Read the census's
UNCENSUSED RESIDUAL line first, then come here for where the residual came from.

## 5. Recipe (c) — diff two builds

```sh
cd <the Runtime/ both binaries should share>
HWPROF_OUT=/tmp/ab ../scratchpad/hwprof_ab.sh /path/binA /path/binB 8 5743 greets 20
```

Interleaves ABBA over N rounds so a load ramp cannot land on one arm. **Every
run's raw `[DPROF]` table is written to `$OUT/` before anything is parsed**, so a
reporting bug costs a re-parse (`hwprof_ab_report.py $OUT`) and never a re-run —
these runs are minutes long on a shared box.

A commit-to-commit A/B must run **both binaries against one asset tree**, or it
is not a matched pair. The pattern that works: `git worktree add <dir> <commit>`,
build, copy the binary *outside the repo*, then rebuild the other arm in the same
worktree and run both from that worktree's `Runtime/`.

To put `--hw_prof` on a commit that predates it, `scratchpad/hwprof_ab.sh`'s
sibling `apply_hwprof.py` pattern (copy the instrumented `TailProf.h` + seven
one-line `Stamp` substitutions + the `.def` append) applies cleanly to any commit
where `TailProf.h` is unchanged.

---

## 6. Reading IPC when you cannot have a miss rate

`scratchpad/hwprof_ladder.c` calibrates what IPC *means* on this machine:
a compute anchor with no memory traffic, and pointer chases (serialised
dependent loads, zero memory-level parallelism) sized to sit in L1, L2, SLC and
DRAM.

```
clang -O2 -o /tmp/ladder scratchpad/hwprof_ladder.c && /tmp/ladder
```

Measured on the M2 Max (load ~11, which is why these are ratios):

| anchor | IPC | latency |
|---|--:|--:|
| COMPUTE, 8 independent FP chains, no memory | **2.42** | — |
| L1 pointer chase, 24 KB | **0.96** | 2.5 ns/hop |
| L2 pointer chase, 1 MB | **0.14** | 30.6 ns/hop |
| SLC pointer chase, 16 MB | **0.03** | 336 ns/hop |
| DRAM pointer chase, 512 MB | **0.01** | 229 ns/hop |

Two honest caveats on that table:

* **The compute anchor is not a ceiling.** 2.42 is what *that* dependent-FMA
  pattern reaches; the M2 P-core is 8-wide and real code with more instruction-level
  parallelism can and does exceed it (the greets lighting wave measures ~3.5).
  Read 2.42 as "comfortably compute-bound", not as a maximum.
* **The chases are the worst case, not the typical one.** A pointer chase has
  zero MLP, so it isolates raw load latency. Real code overlaps misses, and sits
  well above the chase figure for the same working set. The ladder brackets the
  regimes; it does not interpolate between them.

**What IPC can prove.** A before/after on the *same* kernel, where the change
touches fewer bytes: instructions roughly flat and cycles down ⇒ the machine
stalled less ⇒ the memory change is the cause. That is a clean, load-robust
adjudication and it is what §5 is for.

**What IPC cannot prove.** An absolute memory-boundedness score comparable
*across different code*. IPC conflates cache stalls with every other stall and
with instruction mix. A high IPC does rule out *domination* by serialised misses
(you cannot retire 3.5 instructions per cycle while the ROB is starved on L2
round-trips) — that direction of the inference is sound, and §7 of
OPTIMIZATION_BACKLOG uses it. The converse is not.

---

## 7. Signposts

`--hw_prof` also brackets every phase with an `os_signpost` interval on subsystem
`com.revival.fds`, category `PointsOfInterest`. On a machine with Xcode these
land on Instruments' Points of Interest track and let it attribute *its* counters
per stage. Headless, read them back with:

```sh
/usr/bin/log show --signpost --style compact \
  --predicate 'subsystem == "com.revival.fds"' --last 5m
```

(`log` is shadowed by a shell function in this user's profile — use the absolute
path.) Output carries per-thread TIDs and µs timestamps.

All intervals share the literal name `"phase"` and carry the real phase name as
the message, because `os_signpost` requires a **compile-time-literal** interval
name and every phase name in `TailProf.h` is a runtime `const char*`. Instruments
groups by message.

Signposts are skipped entirely unless a tracing tool is attached
(`os_signpost_enabled`), so they cost nothing in an ordinary `--hw_prof` run.

---

## 8. Gotchas

* **Load.** Other agents bench concurrently on this box; `uptime` before every
  recording, and report it. Loads of 12–18 were normal during this work. Counter
  **ratios** (IPC, instructions/pixel) are far more load-robust than any ms
  column — a descheduled worker burns wall time but retires nothing.
* **Discard run 1 after a rebuild.** First-touch and page-in noise.
* **Never rebuild while a DEMO runs** — the running binary gets replaced, exit
  code 137. Recover with `codesign -f -s - Runtime/DEMO`.
* **Keep A/B binaries outside the repo.** `git stash -u` will happily swallow a
  binary you parked in `Runtime/`.
* **A resigned binary is still the same binary here** — `codesign -f -s -` does
  not change the measured numbers, but it does invalidate any prior code-signing
  cache, so the first run after resigning is slower. Discard it.
* **`--snapshot` cannot give a phase breakdown** — it is all warmup. Use
  `--bench=scene@scene=<s>,t=<t>,iters=N`.

---

## 9. Worked example — the cones call, 2026-08-12

Recorded and exported exactly as §1a describes; that section owns the
mechanics (`DEVELOPER_DIR`, `xctrace record`, the XML id/ref trap). This
section is only the result, because it is the largest single perf finding
in the tree and the reasoning is worth keeping.


The pass the whole campaign was aimed at, at city `t=1961`, 1920×1080, 12 workers.

**Per-symbol (Time Profiler, running-state samples, steady-state window):**

| symbol | self | 
|---|---|
| `Render_VolumetricCones_Tile` | **37.5 %** |
| `Render_DeferredLighting_Tile_OuterVec` | 15.1 % |
| everything else | < 5 % each |

Cones is ~100 % *self* time — the kernel is one fully-inlined monolith, so the
call tree bottoms out there and per-symbol profiling cannot go deeper. To go
deeper you need either source-line attribution or ablation (below).

**The arithmetic (via `FDS_CONE_ATTR=1` and a `-DFDS_CONE_DIAG=1` build):**

```
46 spots -> 1187 tile-entries over 96 cone tiles (12x8) = 12.4 spots/tile
1187 tile-entries x 21600 px/tile = 3.20 M (8px-batch x spot) pairs/frame
                                  = 25.6 M (lane x spot) scalar solves/frame
```

**Splitting the instruction budget by ablation** — keep the scalar prologue,
`continue` before everything downstream, sink the result so it is not
dead-coded, and diff `Ginstr/f`. Instruction counts are *deterministic*
(identical to 4 significant figures across runs) and immune to the load that
makes wall times useless here:

| arm | Ginstr/f | share |
|---|---|---|
| full cones pass | 4.217 | 100 % |
| scalar per-lane quadratic solve only | **2.681** | **63.6 %** |
| SIMD body + shadow taps + accumulate | 1.536 | 36.4 % |

**Conclusion: the cost is not the integral, it is the prologue.** The per-lane
quadratic solve is scalar, sits inside the per-spot loop, and runs 25.6 M times
a frame at ~105 instructions each (~165 per *alive* lane) — 64 % of the pass,
~24 % of every instruction the frame retires, feeding a SIMD body that is 8-wide.

**IPC on the cones phase is 4.0–4.2**, so this is a pure instruction-*count*
problem: the loop is not stalling, not memory-bound, and there is nothing for a
cache-miss rate to explain. Do not go looking for one here.

### The t-sweep — this is not a `t=1961` anomaly

`t=1961` is the worst *frame*, but the cones share is a property of the whole
city scene. `Ginstr/f` for the `cones` phase against `renderFrame`'s, 6 iters
each (instruction ratios, because wall was useless at load average 30):

| city `t` | cones Ginstr/f | renderFrame Ginstr/f | **cones share** | IPC | cones wall_min |
|---|---|---|---|---|---|
| 400  | 2.822 | 6.372 | 44 % | 4.00 | 21.2 ms |
| 900  | 2.717 | 6.609 | 41 % | 4.00 | 22.1 ms |
| 1400 | 1.547 | 5.147 | **30 %** | 3.96 | 12.5 ms |
| **1961** | **4.150** | **8.615** | **48 %** | 4.03 | 31.6 ms |
| 2400 | 2.137 | 4.501 | 47 % | 3.51 | 29.3 ms |

**30–48 % of every instruction the frame retires, everywhere in the scene.** The
original campaign figure ("~50 % of every instruction retired" at `t=1961`)
reproduces at 48 % and is not a pose artifact. IPC stays 3.5–4.0 across the
sweep, so no pose turns this into a memory-bound problem either.

### Hypotheses this killed, with numbers

* **"the narrow-cone 8-segment hybrid dominates"** — no: `narrow(seg8)=0`. All
  46 city headlights are wide cones and take the *cheapest* closed-form branch.
* **"scalar per-lane shadow taps dominate"** — no: `shadowed=0` in city. Not one
  headlight casts.
* **"finer cone tiles would tighten the cull"** — weak, and it is the *cull*
  that is weak, not the tiling: 6x4 -> 12x8 moves total (px x spot) only
  29.2 M -> 25.6 M, and disabling `spot_cone_cull` entirely at 12x8 moves
  tile-entries just 1187 -> 1384. The cone-vs-tile test rejects **14 %**; the
  range-sphere screen rect before it already did the heavy lifting
  (46x96 = 4416 -> 1384). Headlight cones genuinely cover large screen area.
* **"hoist the redundant `1.0f/uV` divide out of the per-spot loop"** — `uV`
  depends only on the pixel, so this looked like ~20 M free divides. Measured
  **instruction-neutral, in fact +0.4 % (4.276 -> 4.294 Ginstr/f)**, byte-null
  but worthless; the compiler was already handling it and the explicit array
  added stores. Reverted. *This is why levers get measured before they get
  believed.*
* **"cull the dead pairs"** — capped low. 39.9 % of (batch x spot) pairs have
  zero alive lanes, but those lanes bail at the cheap sphere test long before
  the divides, so they are not where the 2.681 G sits.

### The lever that was left — TAKEN, 2026-08-12b, `--vol_cone_solve_vec`

Vectorize the per-lane quadratic solve to 8 wide: **2.681 G of 4.217 G
instructions**, one lane per iteration feeding a body that was already 8-wide.
The standing comment in the source (*"the a-sign branching is too hairy to
vectorize cleanly"*) was the reason it was never done, and the three-way sign
branch on `a` is genuinely the hard part — but both live arms compute the same
`disc`/`sqrt`/`1/(2a)` shape and differ only in which root interval they take,
which is a blend. Done, default ON, **and the city pin did not move.**

**The shipping arm, `city t=1961`, `iters=6`, interleaved ABBA min-of-6 on one
binary (the flag is the only difference between arms):**

| | scalar solve | 8-wide | |
|---|--:|--:|--:|
| cones wall_min | 30.764 ms | **21.406 ms** | **−9.36 ms, −30.4 %** |
| cones Ginstr/f | 4.091 | **2.868** | −29.9 % |
| cones Gcyc/f | 1.020 | **0.718** | −29.6 % |
| renderFrame wall_min | 71.753 ms | **62.309 ms** | **−9.44 ms, −13.2 %** |
| renderFrame Ginstr/f | 8.288 | 7.067 | −14.7 % |
| renderFrame Gcyc/f | 2.199 | 1.918 | −12.8 % |

The frame saving (−9.44 ms) and the cones saving (−9.36 ms) agree, which is the
internal check that the phase attribution is real. IPC is flat (3.91 → 3.94),
which is the *point*: this removed instructions from an instruction-bound loop,
exactly as the diagnosis said it would, rather than unblocking a stall.

**The variant comparison** below was run on the pre-gate build as a matched
4-arm A/B (one binary set, one asset tree, 6 rounds with the arm order rotating
each round, load 12.5 → 18.9). City's cones are all wide, so the later
segmented-cone gate does not move these numbers — the shipping arm reads 2.886
there against 2.868 above, inside the counter's reproducibility.

| arm | cones wall_min | cones Ginstr/f | cones Gcyc/f | IPC |
|---|--:|--:|--:|--:|
| scalar solve | 34.694 ms | 4.167 | 1.024 | 3.96 |
| **8-wide, bit-exact (SHIPS)** | **25.410 ms** | **2.886** | **0.722** | 3.90 |
| 8-wide + range-sphere early-out | 26.340 ms | 2.944 | 0.717 | 3.98 |
| 8-wide, relaxed FP association | 25.449 ms | 2.871 | 0.714 | 3.94 |

**Achieved against ceiling, stated honestly.** Holding the ablation's split
(the SIMD body is untouched, so ~1.52 G of the 4.167 G baseline), the solve went
**2.65 → 1.37 Ginstr/f, a 1.94×**, not the 8× the lane count suggests. Two
reasons, and the second is the one that generalises:

* A perfect 8-wide port would remove 2.32 G; this removed 1.28 G, **55 % of
  that ideal**. But on arm64 there is no 8-wide unit — simde emulates every
  `__m256` op with **two** 128-bit NEON ops, so the floor for a `_mm256_`-spelled
  port is ¼ of scalar, not ⅛. Against that realistic floor the port achieves
  **64 %**.
* The rest is the early-out the scalar arm gets for free: its dead lanes
  `continue` at the cheap sphere test, while the wide arm computes both `a`-sign
  branches and the whole tail for all eight. **Reclaiming it was tried and it
  does not work** — see the rejected arms below.

### Three variants built, measured and rejected — with their numbers

Each was a real build, benched in the same interleaved session as the arm it is
compared against, so the comparison is matched rather than cross-session. For
scale: `Ginstr/f` reproduces to **0.3 %** for a fixed binary+arm across sessions
(the shipping arm read 2.886 / 2.891 / 2.894 in three), so a 1.5–2 % move is a
real effect, not noise.

* **Range-sphere early-out** (`FDS_CONE_SOLVE_EARLYOUT`, compiled out in place).
  `movemask` the alive mask right after the sphere test and `continue` when no
  lane survives. Premise: 39.9 % of (8px-batch × spot) pairs have zero alive
  lanes. **Measured +2.0 % instructions (2.886 → 2.944) — it costs.** The
  premise is what is wrong: the branch only fires when *all eight* lanes miss
  the sphere, and most dead pairs lose their lanes later (a-sign, or `zHi<=zLo`
  in the tail), so it fires too rarely to pay for the test.
* **Raw `rcp`/`rsqrt` instead of true div/sqrt** (`FDS_CONE_SOLVE_APPROX`,
  compiled out in place). The solve has the only non-pipelined ops in the loop —
  3 divides + 2 square roots per (8px-batch × spot). **Measured +1.6 %
  instructions (2.891 → 2.936), −1.7 % cycles, −1.0 % wall: no win.** Two
  reasons. The loop is instruction-bound (IPC 3.9), not latency-bound; and on
  NEON `vrecpe`/`vrsqrte` are **8-bit** estimates, half the 12 bits the x86
  intrinsic names imply, so anything usable needs Newton-Raphson steps that cost
  *more* instructions than the divide they replace. Raw is the fastest this
  family can be, so measuring raw closes the whole family in one build — and it
  is the cheap discriminator to reach for first. (Its output was also nearly
  harmless: **200 px of 2 073 600 at 1 LSB**. So the `vec_ggx` plateau argument
  never had to be litigated here — a volumetric integrand really does tolerate
  approximation far better than a `reflect()` direction. Approximation simply
  does not pay in this loop.)
* **Relaxed FP association** — `_mm256_min_ps`/`_mm256_max_ps` and
  `_mm256_fmsub_ps` instead of the exactness-preserving spellings. **Measured
  −0.5 % instructions (2.886 → 2.871)**, and it moves the city pin: **3 px at
  1 LSB**. Half a percent is not worth giving up a byte gate.

### The reusable technique: read the contraction map off the disassembly

This is the part worth keeping. Under the tree-wide `-ffp-contract=fast` the
compiler decides, for each `a*b + c*d`, **which product gets fused and which gets
rounded**, and *its choice does not follow the source order.* Reasoning from the
source will not reproduce it. Read it off the binary:

```sh
nm -n build/DEMO/DEMO | grep VolumetricCones_Tile        # addr + the next symbol
objdump -d --start-address=0x… --stop-address=0x… --no-show-raw-insn build/DEMO/DEMO
```

Release is thin-LTO, so the `.o` files are bitcode — **disassemble the linked
binary, not the object.** On arm64 read `FMADD Sd,Sn,Sm,Sa` as `Sa + Sn*Sm` and
`FNMSUB Sd,Sn,Sm,Sa` as `Sn*Sm - Sa`; a bare `FMUL` feeding an `FADD`/`FSUB` is a
product the compiler chose to **round**. The map it had chosen here, which the
source does not suggest anywhere:

```
DP  = fma(Pz,Dz, fma(Px,Dx, fl(Py*Dy)))       from  Dx*Px + Dy*Py + Dz*Pz
VP  = fl( Pz + fma(Px, X, fl(Y*Py)) )          Y*Py hoisted per (row × spot)
DV  = fl( Dz + fma(Dx, X, fl(Y*Dy)) )
sD  = fma(VP,VP, -fl(sphereC*uV))
a   = fma(DV,DV, -fl(c2*uV))
b   = t+t,  t = fma(c2,VP, -fl(DP*DV))
disc= fma(b,b, fl(cq * fl(a * -4)))            note -4, not "4·a·cq" subtracted
```

In every pair it is the **second** product that ends up rounded. Two traps when
transcribing that into intrinsics:

* **simde does not give you the fused op you asked for.** `_mm256_fmadd_ps`
  lowers to `vfmaq_f32` (a true FMA), but `_mm256_fmsub_ps` and
  `_mm256_fnmadd_ps` lower to `sub(mul(a,b), c)` — which hands the fusion choice
  straight back to the compiler. Spell every `a*b - c` as `fma(a, b, NEG(c))`
  with `NEG` an explicit sign-bit xor: an operand of an FMA *intrinsic* is a
  barrier the compiler will not re-contract across, so the rounded product is
  pinned by construction.
* **`std::min`/`std::max` are not `_mm256_min_ps`/`_mm256_max_ps`.** NEON `FMIN`
  resolves NaN and signed zero the opposite way from the scalar `FCSEL` the
  compiler emits. Transcribe them as cmp + blend. Likewise use the *unordered*
  compare predicates (`_CMP_NLT_UQ`, `_CMP_NLE_UQ`) wherever the scalar reads
  `if (x < y) continue;` — that is its exact negation, NaN included.

Verified end to end: `city 3cbe42b1…` 2/2 with the flag on **and** 2/2 with it
off, `greets 778fa6ac…` 2/2, `fountain 8db68ccb…` 2/2, `render_gate` 3/3 PASS.

### Where cones run, and the t-sweep after

The pass is **not** in every pinned scene, so say which pin proves what:

| scene | cones Ginstr/f | what its pin proves here |
|---|--:|---|
| city t=1961 | 4.09 → 2.87 | the change itself |
| greets t=1588 | 1.18 → 1.18 | cones DO run, on the segmented branch — gated out, see below |
| fountain t=2500 | 0.000 (0.003 ms) | no cones — a no-regression control only |

**Greets is why the wide arm is gated to non-segmented cones.** Ungated at the
greets pin pose the 8-wide solve reads **instructions −3.8 % but cycles +3.6 %
and wall +1.4 %** (7.63 → 7.73 ms), and that reversal reproduced in two
independent interleaved min-of-6 sessions, so it is not noise. The mechanism:
greets' cones are the narrow disco beams, which take the 8-segment hybrid body —
the solve is a minor share of that phase, and the wide arm's unconditional work
on dead lanes is not repaid there. With `!segPath` in the gate greets is neutral
(1.184 → 1.183 Ginstr/f, wall −1.6 %) and city keeps the entire win, because
every city headlight is a wide cone. Both arms are bit-identical, so the gate
costs nothing to get wrong in the other direction. **The general lesson: a
per-lane→wide port is not uniformly a win across call sites with different
bodies — measure the second scene before defaulting it on.**

City t-sweep, same poses as the sweep above, interleaved, 2 rounds (load 27–32):

| city `t` | cones Ginstr/f | | cones wall_min | | cones share of frame |
|---|--:|--:|--:|--:|--:|
| | scalar | 8-wide | scalar | 8-wide | scalar → 8-wide |
| 400  | 2.823 | 2.061 (−27.0 %) | 28.8 ms | 22.2 ms | 46 % → 38 % |
| 900  | 2.729 | 1.992 (−27.0 %) | 23.9 ms | 17.7 ms | 41 % → 33 % |
| 1400 | 1.554 | 1.181 (−24.0 %) | 16.6 ms | 12.5 ms | 32 % → 27 % |
| **1961** | **4.170** | **2.894 (−30.6 %)** | 38.0 ms | 30.8 ms | **50 % → 41 %** |
| 2400 | 2.144 | 1.499 (−30.1 %) | 19.4 ms | 13.7 ms | 49 % → 40 % |

**Cones is still the biggest single item in the frame** — 2.886 of 7.090
Ginstr/f at t=1961, against `DeferredLighting` 1.247, `fastfog` 1.092, `gbuffer`
0.891, `TBR-render` 0.846. What changed is what it is *made of*: the untouched
SIMD body + shadow taps + accumulate is now the majority of it (~1.52 of 2.89 G
— **inferred** by holding the ablation's split, not re-measured). So the next
lever inside this pass is the integration body, not the prologue, and the next
person here should re-run the a16567b ablation against the new arm before
believing that split.
