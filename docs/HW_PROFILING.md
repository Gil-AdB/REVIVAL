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

---

## 10. Worked example round 2 — re-ablating the cone pass, 2026-08-12c

§9 closed with an instruction: *the untouched integration body is now the
majority of the pass (~1.52 of 2.89 G — **inferred** by holding a16567b's split,
not re-measured); re-run the ablation against the new arm before believing that
split.* This section is that re-run. **The inference was wrong in both
directions, and it was blind to a fifth of the pass.**

### The ladder is now committed, not ad-hoc

`-DFDS_CONE_ABLATE=n` in `FDS/RENDER/DeferredVolumetric.cpp` (compile-time only,
`n=0` emits nothing) puts a staged `continue` in the per-spot loop:

| n | what is KEPT |
|---|---|
| 1 | cut at the top of the per-spot loop — the per-batch floor |
| 2 | + the per-spot scalar prologue |
| 3 | + the 8-wide cone-interval solve |
| 4 | + the scalar per-lane dz/fade loop |
| 5 | + the broadcasts and the whole integration body |
| 0 | the full pass |

Each cut sinks what it retains into a **per-tile** `__m256` accumulator drained
by one `volatile` store per tile call. That detail is not decoration: sinking
per *batch* would false-share one cache line across 12 workers and wreck the
cycle column, and *not* sinking lets the compiler delete exactly the work you
are trying to price. Sink cost is 2–7 instructions per (batch × spot).
`scratchpad/cone_ablate.sh` drives it — rewrite the define, rebuild the one TU,
relink, bench, discard run 1.

### The re-measured split — city t=1961, shipping (8-wide-solve) arm

Two independent sessions, agreeing to **0.3 %** (2.869 / 2.866 G total):

| slice | Ginstr/f | share |
|---|--:|--:|
| per-batch floor (prologue + composite, no spot loop) | 0.047 | 1.6 % |
| per-spot loop + scalar prologue | 0.167 | 5.8 % |
| **the 8-wide cone-interval SOLVE** | **0.992** | **34.6 %** |
| **scalar per-lane dz / fade-window loop** | **0.270** | **9.4 %** |
| **broadcasts + integration BODY** | **1.089** | **38.0 %** |
| **per-lane colour ACCUMULATE** (incl. the composite write) | **0.301** | **10.5 %** |
| total | 2.866 | 100 % |

**What the inference got wrong.** It predicted solve 1.37 G and body ~1.52 G.
Measured: solve **0.992**, body **1.089**. And the number that matters — the old
split had a single bucket called *"SIMD body + shadow taps + accumulate"*, so it
could not see that **0.571 G (19.9 % of the pass) sits in two SCALAR per-lane
loops**, one on each side of the 8-wide body. Holding an old ratio over a
changed kernel is not a measurement; this is why §9 said to re-run it.

**The body sub-ladder (stages 6–10, cutting inside the analytic branch) is NOT
trustworthy and is recorded here only so nobody repeats it.** It came out
non-monotonic — stage 8 (2.092 G) reads *lower* than stage 7 (2.185 G) while
keeping strictly more source. Cutting inside a single basic block lets the
compiler re-schedule and re-DCE around the sink, so the increments stop being
differences of nested supersets. Ablation is reliable at *statement-group*
granularity, where each cut removes a whole dependency chain; it is not reliable
at expression granularity. One thing did survive it, as a **kill**: the per-lane
`noiseBuf` loop measures **+0.015 G**, i.e. ~4.7 instructions per (batch × spot)
against the ~64 an un-hoisted scalar loop would cost — the compiler already
hoists it out of the per-spot loop (it depends only on `pxHashArr`, which is
per-batch). *Do not "fix" the noise loop; it is already fixed.*

### The lever taken — `--vol_cone_lane_vec`, both leftover loops 8 wide
*(flag deleted 2026-08-29: it had been default ON and bit-exact since `03ef0ff0`;
both loops are unconditional now. `--no-vol_cone_lane_vec` arms quoted below no
longer exist.)*

Same defect at both ends of the body, and it is **memory traffic, not
arithmetic**: the solve writes `zLoArr`/`zHiArr` as vectors, a scalar loop reads
them back a lane at a time to write three more stack arrays, and the 8-wide body
immediately reloads those as `__m256`; symmetrically `accV` is already an
`__m256` and gets spilled to `accArr` purely so eight scalar iterations can
load-modify-store three more arrays that the composite then reloads. The colour
accumulators now live in **registers across the whole per-spot loop** and drain
to `accB/accG/accR` once per batch, so the composite loop is untouched.

**MEASURED — AGAINST A REAL PARENT-COMMIT BINARY, NOT AGAINST THE OFF ARM.**
city t=1961, `iters=6`, three binaries/arms interleaved round-robin min-of-6,
load 16–22. The parent binary is `7e34645` built in the same worktree against the
same asset tree:

| cones @ t=1961 | wall_min | Ginstr/f | Gcyc/f | IPC |
|---|--:|--:|--:|--:|
| parent `7e34645` | 21.387 ms | 2.866 | 0.701 | 4.006 |
| new, flag OFF | 24.576 ms | 2.941 | 0.808 | 3.566 |
| **new, flag ON (ships)** | **17.145 ms** | **2.390** | **0.555** | **4.192** |

**Achieved vs the parent commit: cones −19.8 % wall (−4.24 ms), −16.6 %
instructions, −20.8 % cycles; `renderFrame` 62.269 → 58.911 ms (−3.36 ms,
−5.4 %), 7.063 → 6.587 Ginstr/f.** The frame instruction saving (−0.476 G) and
the cones instruction saving (−0.476 G) agree exactly — the attribution check.

### The OFF arm is NOT a stand-in for the parent, and that is a method finding

The one-binary ABBA A/B — the format §9 used — reads **−30.8 % wall / −18.8 %
instructions**. That **overstates the shipped gain**, because its baseline is
this binary's OFF arm, which is itself **+2.6 % instructions and +15.3 % cycles
worse than the parent, with IPC collapsing 4.006 → 3.566**. Merely compiling the
vector path in alongside it degrades the scalar path's register allocation and
scheduling.

It is specifically the *scalar* path that suffers: a control build with
`laneVec` folded to a compile-time `true` (scalar arm dead-coded away) measures
the **shipping** arm at 2.371 G vs 2.390 G — a dual-arm tax of only **+0.8 %
instructions, cycles within noise**. So the fallback costs the shipping arm
essentially nothing and stays in for A/B, but *it must not be used as the
baseline*.

Against the ablation's prediction: the two loops were 0.571 G and the measured
removal is **0.476 G, i.e. 83 % of them** — the vector replacements cost ~0.095 G,
not the ~0.02 G a naive reading of the one-binary A/B (−0.552 G) would suggest.

> **Carry this forward.** A one-binary flag A/B prices *the flag*, not *the
> commit*. When the change adds a second code path to a hot function, confirm
> against a binary built from the parent commit before quoting a delta. **§9's
> own headline numbers for `--vol_cone_solve_vec` were taken in the one-binary
> format and have not been re-checked this way** — that is flagged as an open
> item, not as a claim that they are wrong.

**This one is a different mechanism from the 8-wide solve, and the counters say
so.** The solve was pure instruction count: IPC stayed flat at 3.9 and cycles
fell in step with instructions. Here, parent → shipping, **cycles fall 20.8 %
against instructions 16.6 %, and IPC rises 4.006 → 4.192** — so the loads and
stores were *stalling* as well as retiring, though only modestly. (Measured
against the OFF arm the same effect reads far bigger — cycles −30 % vs
instructions −19 %, IPC 3.49 → 4.10 — but that is mostly the OFF arm's own
IPC collapse, which is why the parent-relative numbers are the ones quoted.) A static histogram of the shipping kernel is **894 `ldr` +
583 `str` of 4475 instructions (33 %)**, which is what pointed at the two loops
in the first place; but note the static count barely moves after the fix (both
arms are still compiled in), so **the static histogram is a hypothesis
generator, not the evidence.** The evidence is the cycle/IPC split above.

**IT IS BIT-EXACT, and unlike the solve it cost nothing to make so.** Neither
loop contains an `a*b + c` that `-ffp-contract=fast` could fuse ambiguously, so
there was no contraction map to read off the disassembly this time. Only three
of the spellings §9 catalogued mattered, and following them worked first try:
`std::max(d, fwMin)` as cmp+blend rather than `_mm256_max_ps`; the **unordered**
predicates `_CMP_NEQ_UQ` / `_CMP_NLE_UQ` as the exact negations of
`if (alive == 0) continue` and `if (acc <= 0) continue`; and the multiply order
kept as `((acc*density)*nNorm)*coneGain` instead of folding the three scalars
into one factor, which would be a re-association.

### Greets needs NO gate here, and that is a measurement

§9's headline caution was that a per-lane→wide port is not uniformly a win
across call sites, and the 8-wide *solve* had to be gated `!segPath` because
greets regressed on cycles and wall. **That gate is not needed for this change,
and greets was measured rather than assumed** — interleaved min-of-6 at the pin
pose:

| greets t=1588 | scalar | 8-wide |
|---|--:|--:|
| cones wall_min | 7.995 ms | 7.640 ms (**−4.4 %**) |
| cones Ginstr/f | 1.165 | 1.143 (−1.9 %) |
| cones Gcyc/f | 0.255 | 0.249 (−2.4 %) |

All three counters move the same direction. The mechanism is why: the solve's
ungated regression came from the wide arm computing both `a`-sign branches and
the whole tail **for dead lanes** that the scalar arm bailed on early. Neither
of these two loops speculates on dead lanes — they do the same per-lane work as
before and only delete the stack round-trip — so there is no dead-lane tax to
repay on the segmented-cone branch. *Same technique, opposite gating answer,
because the mechanism differs.*

### City t-sweep, interleaved, 2 rounds

| city `t` | cones Ginstr/f | | cones wall_min | | share of frame |
|---|--:|--:|--:|--:|--:|
| | scalar | 8-wide | scalar | 8-wide | scalar → 8-wide |
| 400  | 2.071 | 1.815 (−12.4 %) | 16.9 ms | 13.1 ms (−22.5 %) | 38 % → 35 % |
| 900  | 2.007 | 1.734 (−13.6 %) | 16.2 ms | 12.4 ms (−23.6 %) | 34 % → 30 % |
| 1400 | 1.188 | 1.017 (−14.4 %) | 9.9 ms | 8.1 ms (−18.3 %) | 27 % → 24 % |
| **1961** | **2.941** | **2.391 (−18.7 %)** | 24.6 ms | **17.4 ms (−29.2 %)** | **41 % → 36 %** |
| 2400 | 1.506 | 1.298 (−13.8 %) | 12.3 ms | 9.8 ms (−20.2 %) | 40 % → 37 % |

Wall falls 18–29 % at every pose, instructions 12–19 %.

### Where the pass stands now, and the next lever

At t=1961 cones is **2.390 of 6.588 Ginstr/f** — still the biggest single item
(`DeferredLighting` 1.247, `fastfog` 1.092, `gbuffer` 0.891, `TBR-render`
0.846), but it is now **36 % of the frame's instructions instead of 41 %**, and
against the parent commit its wall is 21.4 → 17.1 ms. Across the campaign's two
rounds the pass has gone 4.217 → 2.390 Ginstr/f, a **43 % cut**, measured
consistently at the same pose.

Scaling the ladder to the new arm, the remaining composition is roughly: the
**integration body ~1.09 G** (now clearly the largest slice, and already 8-wide),
the **solve ~0.99 G**, the per-spot prologue 0.17 G, the floor 0.05 G. Two
things a next round should know:

* **The body is the target, and the sub-ladder above cannot resolve it.** It
  needs a different instrument than staged `continue`s — source-line sampling,
  or splitting the body into real functions so per-symbol attribution works.
* **Levers already dead, with numbers, do not re-try them:** the per-lane noise
  loop (+0.015 G, already hoisted by the compiler); per-spot invariant
  broadcasts (the disassembly carries only **37 `dup.4s` in the entire
  function** against the ~25 `_mm256_set1_ps` written per (batch × spot) in the
  source — the compiler already hoists them out of the loop); and from §9, the
  range-sphere early-out, raw `rcp`/`rsqrt`, relaxed FP association, finer cone
  tiles, and the `1/uV` hoist.

## 11. Worked example round 3 — the stage round-trip, and the counter that stopped moving, 2026-08-13

Round 3 asked one question, the user's: *"for the scalar→simd→scalar — any way
to reorder this so we won't need the round-trip?"* Round 2 had widened the two
scalar per-lane loops but left the **stack arrays** between the stages — the
8-wide solve ends with three `__m256`, spills them to `zLoArr`/`zHiArr`/
`aliveLane`, and the next 8-wide stage loads them straight back; symmetrically
for `dzArr`/`invDzArr`/`fadeStartArr`. The lever was to delete the arrays and
hand the stages over in registers.

**It was built, it is bit-exact, it does exactly what it claims — and it is
worth 0.1 %. It is not shipped.** What the round actually found is bigger than
the lever it was chasing, and it retires the whole *direction*.

### What forces the staging — the three verdicts

**(a) Register pressure, and it is measured, not argued.** The arrays are not a
buffer, they are a **phi node**: two arms (8-wide solve / per-lane scalar solve)
converge, and a memory location is the only place a compiler merges a value
produced element-wise on one path and vector-wise on the other. Deleting them
works — read it off the disassembly, `FDS_CONE_FORCE=1` builds with the arms
folded so only the shipping path survives:

```
pre-fusion, at the solve's tail          fused, same site
  and.16b v5, v4, v7                       and.16b v19, v4, v18
  and.16b v6, v3, v16                      and.16b v21, v3, v16
  str     q6, [sp, #0xaf0]                 and.16b v2,  v4, v17
  str     q5, [sp, #0xae0]                 and.16b v7,  v3, v7
  and.16b v5, v4, v10                      ldr     q29, [sp, #0x660]
  and.16b v6, v3, v19                      and.16b v0,  v4, v29
  str     q6, [sp, #0xad0]                 str     q0,  [sp, #0x7c0]
  and.16b v6, v4, v0                       and.16b v0,  v3, v29
  str     q5, [sp, #0xac0]                 str     q0,  [sp, #0x7b0]
  and.16b v5, v3, v0                       ldr     q14, [sp, #0x670]
  str     q5, [sp, #0xab0]                 ldr     q15, [sp, #0x480]
  str     q6, [sp, #0xaa0]
```

Six `str q` — three `__m256`, two NEON halves each — become **two**. Four of the
six really are gone. And look at what sits in the gap: three `ldr q` refilling
*other* spilled values. Over the whole per-spot loop the net is **stack `str q`
116 → 115, `ldr q` 248 → 263**. The allocator re-spends every register the
fusion frees, immediately, on something else. The live set does not fit and did
not fit before; the arrays were never the reason.

**(b) Loop structure does not force it and FP order is free.** The stages walk
the same order, and fusion keeps the *identical* masked `__m256` — the same
`_mm256_and_ps(zLo, mAlive)` that used to be stored is simply kept. So
bit-exactness cost nothing and needed no contraction map: all three scene pins
reproduce **3/3** with the fusion on, first try (city `3cbe42b1…`, greets
`778fa6ac…`, fountain `8db68ccb…`), and `render_gate` passes all four rows
(`mirrortest` / `rttslot` / `conetest` / `halotest`) 3 runs out of 3.

**(c) There ARE two genuine materialization points, and they are why the fusion
cannot be unconditional.** The **scalar** dz/fade loop reads
`zLoArr`/`zHiArr`/`aliveLane` element-wise, so under `--no-vol_cone_lane_vec`
the solve must still publish the arrays; and the segmented-hybrid branch's
per-segment shadow tap reads `aliveLane[ln]`. Fusion is a property of the whole
chain, not of one stage.

### The measurement, and why it does not ship

Single-arm control builds (`FDS_CONE_FORCE=1`, ±`FDS_CONE_FUSE`), city t=1961,
interleaved min-of-6, **two independent sessions**:

| cones @ city t=1961 | Ginstr/f | Gcyc/f | IPC |
|---|--:|--:|--:|
| unfused (`FUSE=0`) | 2.362 / 2.362 | 0.566 / 0.573 | 4.09 / 4.10 |
| fused (`FUSE=1`)   | 2.359 / 2.359 | 0.550 / 0.569 | 4.15 / 4.12 |

Instructions reproduce **exactly** — −0.003 G, −0.1 %, both sessions. Cycles
read −2.8 % then −0.7 %: **the cycle win did not reproduce**, so it is noise, not
a result. Across the city sweep the fusion is −0.1 % to −0.7 % instructions and
−0.3 % to −1.6 % cycles at every pose. Greets (the segmented branch) is the best
case at −1.1 % instructions / −1.2 % cycles / −3.0 % wall, one session.

**And in the binary we actually ship it is a REGRESSION.** The control above is
a build where the scalar fallbacks do not exist. Ship the fusion into the real
two-arm structure — the `--vol_cone_solve_vec` / `--vol_cone_lane_vec` arms are
still there — and the merge points and register variables land on top of a
function that must still support the array path:

| cones @ city t=1961 | Ginstr/f |
|---|--:|
| parent `03ef0ff` | 2.389 / 2.389 / 2.389 / 2.388 |
| fusion shipped unconditionally | 2.437 / 2.438 / 2.436 / 2.437 |

**+2.0 %, every run.** Wrapping it in a runtime flag is worse still: the OFF arm
measures +3.5 % instructions against the parent and the ON arm +2.4 % — round
2's dual-arm tax finding, reproduced at four times the size, because this change
adds branches *inside* the hot path rather than beside it. So there is no form
in which this ships: unconditional costs 2 %, flagged costs 2.4 %, and the thing
it buys is 0.1 %.

> **The answer to the question.** Yes, the stages can be reordered to hand over
> in registers, and it works. But the round trip was never the cost — the
> register file is. It spills the same values under a different name.

### THE FINDING THAT MATTERS: the cone pass is no longer instruction-bound

`FDS_CONE_HOTONLY=1` deletes the arms city never executes — the segmented
hybrid, the ray-march fallback, the midpoint shadow tap — leaving only the
branch its wide headlights take. **City is byte-identical under it** (city pin
`3cbe42b1…` reproduces on the diagnostic binary), which is what makes it a clean
price for *interference from code that never runs*. City t-sweep, min-of-3:

| city `t` | instr, cold arms removed | cycles | wall |
|---|--:|--:|--:|
| 400  | **−7.4 %** | −2.0 % | −2.4 % |
| 900  | **−7.8 %** | −1.4 % | −0.8 % |
| 1400 | **−8.4 %** | −1.7 % | −0.2 % |
| 1961 | **−10.5 %** | **+0.5 %** | +1.2 % |
| 2400 | **−8.0 %** | −0.6 % | +0.9 % |

**8–10 % of every instruction the pass retires is the cost of carrying arms the
scene never takes** — spill code, hoisted setup, branch tests. And removing them
buys **zero cycles and zero wall**. IPC just falls, 4.11 → 3.64.

The counter is not broken and this is not a measurement artifact — on the same
binaries a *known-large* change still moves cycles hard: parent with
`--no-vol_cone_lane_vec` reads 2.939 G / 0.845 Gcyc against 2.387 G / 0.589 Gcyc,
i.e. +23 % instructions costing +43 % cycles. **Not all instructions cost the
same.** Round 2's 0.55 G were dependent scalar load-modify-store chains and cost
0.26 Gcyc. These 0.25 G are well-scheduled spill and branch code that dual-issues
into slack and costs nothing.

> **Carry this forward — it retires a direction.** Rounds 1 and 2 worked because
> the pass was instruction-bound (a16567b measured IPC 4.0–4.2 and called it
> "instruction COUNT — not stalls, not memory", correctly, *at the time*). After
> a 4.217 → 2.390 Ginstr/f cut the pass has **crossed over**. Marginal
> instructions in the current arm are free. **Stop counting instructions on this
> pass.** The next win has to come from cycles — the dependency structure, the
> non-pipelined `fdiv`/`fsqrt`, or doing less work (fewer pixels × spots) — and
> the instrument for it is not `Ginstr/f`.

### The two instruments this round adds, and their cost

Both are compile-time and both emit **literally nothing** at their default 0 —
verified, not assumed: the shipping binary's cone kernel disassembles to the
**identical histogram** as the parent's (4538 instructions, 334/210 stack `ldr`/
`str` q, 479/389 scalar, 210 `fmul.4s`, 114 `fmla.4s`, 39 `dup.4s`) and measures
2.388–2.390 Ginstr/f against the parent's 2.387–2.390.

* **`-DFDS_CONE_FORCE=1`** — folds `vol_cone_solve_vec` to compile-time `true`
  so its scalar fallback dead-codes away. (It used to fold `vol_cone_lane_vec`
  too; that flag was **deleted 2026-08-29** and its two loops are now
  unconditionally 8-wide, so the define no longer has anything to do there.) A runtime-flagged
  function carries every arm in **one register allocation**, so its disassembly
  and its pressure are the union of paths no single frame takes. Round 2 needed
  this, built it ad hoc, and could only describe it in prose.
* **`-DFDS_CONE_HOTONLY=1`** — the cold-arm price above. Not a correct renderer;
  valid only for scenes that never reach the deleted branches (city does not).

The rejected fusion is kept reproducible as `scratchpad/cone_fuse.patch` rather
than as dead code in the kernel, because unlike `FDS_CONE_SOLVE_EARLYOUT` /
`FDS_CONE_SOLVE_APPROX` (which are a few lines at one site) it touches eight
sites across 300 lines, and leaving it in `#if` arms would cost the shipping
binary the very 2 % that disqualified it.

## 12. Worked example round 4 — the register-pressure question, answered NO, 2026-08-13c

The user's question was **"any way to rewrite this while relieving register
pressure?"** The premise is exact and worth stating, because it is the reason
the answer is interesting: on arm64 there is no 256-bit unit, simde lowers every
`__m256` op to **two** 128-bit NEON ops, so every live `__m256` value occupies
**two** of the 32 `v` registers and the file is effectively 16 slots deep. The
cone solve holds ~30 live values. It spills. All of that is true.

**Relieving the pressure makes the pass slower.** Measured three ways, and the
mechanism is that the two halves of an `__m256` op are *independent NEON chains*
— the spelling is already unroll-and-jam by 2, and taking it apart costs more
than the spills it saves.

### First: re-read the ablation ladder on the CYCLE column

Round 3 ended with "stop counting instructions on this pass; the instrument is
not `Ginstr/f`" — and then round 2's committed ladder (`-DFDS_CONE_ABLATE=n`,
`scratchpad/cone_ablate.sh`) had never been read on `Gcyc/f`. It takes five
minutes. city t=1961, runs=5, run 1 discarded after each rebuild:

| stage kept | cones `Gcyc/f` | increment | % of pass CYCLES | (% of pass instr) |
|---|---|---|---|---|
| per-batch floor | 0.012 | 0.012 | 2.1 % | 2.2 % |
| + per-spot scalar prologue | 0.044 | 0.032 | 5.5 % | 6.8 % |
| **+ the 8-wide cone-interval SOLVE** | 0.294 | **0.250** | **42.7 %** | 41.6 % |
| + per-lane dz/fade | 0.317 | 0.023 | 3.9 % | 3.0 % |
| + body: broadcasts/quadratic/rsqrt-NR/args | 0.396 | 0.079 | 13.5 % | 17.6 % |
| + body: `atanDiff` | 0.443 | 0.047 | 8.0 % | 6.7 % |
| + body: midpoint cone/fade/softEdge | 0.447 | 0.004 | 0.7 % | −2.6 % |
| + body: `vAcc` chain | 0.537 | 0.090 | 15.4 % | 17.9 % |
| + body: noise / masks / shadow tap | 0.555 | 0.018 | 3.1 % | 1.7 % |
| + colour accumulate | 0.585 | 0.030 | 5.1 % | 5.2 % |

The solve is the single largest bucket on **both** columns, so it is where a
pressure rewrite has to be tried.

### The three spellings of the same solve

`FDS_CONE_W4` (in-tree, default 0, emits nothing) spells the solve as two 4-wide
passes over the same 8-pixel batch. Identical NEON op count by construction —
simde was already emitting two 128-bit ops per `__m256` op — so only the live-set
size moves. Priced on **single-arm control builds** (`-DFDS_CONE_FORCE=1
-DFDS_CONE_HOTONLY=1`, so no dual-arm tax and city-only paths), interleaved
round-robin min-of-6, city t=1961:

| arm | cones wall_min | `Ginstr/f` | `Gcyc/f` | IPC | stack `ldr q`/`str q` |
|---|---|---|---|---|---|
| `__m256`, as shipped | 16.673 ms | 2.114 | 0.576 | 3.651 | 89 / 79 |
| W4, half loop **unrolled** | 16.618 ms | 2.035 | 0.580 (+0.7 %) | 3.504 | 91 / 82 |
| W4, half loop **rolled** | 18.083 ms | 2.162 | **0.619 (+7.5 %)** | 3.478 | **72 / 70** |

Read the third row first. **Rolling the half loop is the only spelling that
actually halves the live set** — left to itself the compiler fully unrolls a trip
count of 2 and schedules both halves together, which restores exactly the 8-wide
live set (row 2: spills go *up*, 89/79 → 91/82). The rolled build gets the
pressure relief the question asks for, `str q` 89/79 → 72/70, and pays **+7.5 %
cycles / +8.5 % wall** for it. In the shipping two-arm binary the runtime-flagged
W4 read −0.1 % instructions / +1.2 % cycles / +2.6 % wall, and merely compiling
the arm in taxed the OFF path **+3.2 % instructions** — which is why it is a
compile-time instrument and not a FeatureFlag.

### Why: 81 % of this core's NEON ALU issue ceiling

Disassemble the solve in the single-arm control (M2 Max, 4 NEON/FP pipes) and
classify. 251 instructions in the solve region, of which **202 are vector ALU**
(78–92 % per 50-instruction bucket), 8 `fsqrt.4s`/`fdiv.4s`, 27 vector
load/store, 22 everything else. The ladder puts the solve at 0.250 `Gcyc/f` and
the DIAG census at 3.20 M (batch × spot) pairs → **~63 cycles per pair against a
~51-cycle vector-ALU-port floor**. The kernel is running at ~81 % of the machine's
NEON issue ceiling.

That single number explains every result on this pass at once:

* **Pressure relief cannot pay.** Spill loads/stores are not the constraint; the
  ALU port is. Removing them (rolled W4) while lengthening the dependency chain
  is a net loss.
* **Round 3's "deleting 7–10 % of instructions moved cycles by zero"** — those
  were scalar and branch instructions, which issue on *other* pipes into slack.
* **More ILP has a small ceiling.** 63 → 51 cycles is −19 % on the solve = −8 % on
  the pass. Unroll-and-jam beyond the free 2× the `__m256` spelling already
  provides is worth at most ~1.4 ms, before counting the spills it would add.
* **The metric is VECTOR-ALU OP COUNT.** Not instructions, not registers.

### What that metric found, and what shipped

Grep the shipping cone kernel for `fmin.4s` / `fmax.4s`: **zero**, at 19 min/max
sites. Two independent reasons, same cost. The solve and the dz/fade loop spell
`std::min`/`std::max` by hand as cmp+blend, because 7e34645 needed bit-exactness
and NEON `FMIN` resolves NaN and −0 the opposite way from the scalar `FCSEL`. And
every `_mm256_max_ps` *already in the body* also lowers to cmp+blend, because
`SIMDE_FAST_NANS` is not defined in this build and simde's NaN-correct fallback
is `m = a<b; (a&m)|(b&~m)`, which LLVM folds to `fcmgt`+`bsl`. The intrinsic that
looks like one op is two.

**`FDS_CONE_NEONMINMAX` (default 1, ships)** routes all 19 through `vmaxq_f32` /
`vminq_f32` on both halves. 33 instructions out of 4538 statically; 2 vector-ALU
ops per site per pair dynamically. Parent-binary vs new-binary, interleaved
min-of-6:

| | cones wall_min | `Ginstr/f` | `Gcyc/f` | IPC |
|---|---|---|---|---|
| parent `67441d86` | 16.922 ms | 2.388 | 0.584 | 4.067 |
| new tree, `NEONMINMAX=0` | 16.924 ms | 2.387 | 0.581 | 4.074 |
| **new tree, default (ships)** | **16.285 ms** | **2.347** | **0.558** | **4.176** |

**−4.5 % cycles, −3.8 % wall (−0.64 ms), −1.7 % instructions.** renderFrame
57.143 → 56.423 ms; the −0.72 ms frame saving matches the −0.64 ms cones saving,
which is the attribution check. The `NEONMINMAX=0` row is the control that proves
the compiled-out W4 arm costs the shipping binary nothing (4538 → 4538
instructions, one `ldr q` of difference).

City t-sweep, min-of-3 per pose: t=400 −4.7 % cyc / −4.4 % wall, t=900 −4.5 % /
−4.8 %, t=1400 −3.2 % / −4.3 %, t=2400 −4.0 % / −4.5 %. **Greets** (the segmented
branch, where the body's `_mm256_max_ps` sites are hot) −3.2 % cyc / −3.5 % wall,
7.595 → 7.330 ms.

**And it is BIT-EXACT in practice**, which was not the expectation — the change
was written as a judge call under the standing "byte-exactness is not required"
rule, with the NaN/±0 tie-break as the known risk. It never materialises: city
`3cbe42b166847e40f7071eedb48d613c`, greets `778fa6acd85a69cf241babefcdaf598e`,
fountain `8db68ccb59416e9a44037e9f387b7bd9` all **3/3**, and `render_gate` **ALL
FOUR rows PASS** including `conetest b41894f9`, which is direct coverage of this
kernel.

### Levers priced and closed this round

* **A pre-solve cull.** New DIAG counter `sphdead`: of 3 204 900 (batch × spot)
  pairs at city t=1961, 39.9 % produce zero alive lanes but only **7.7 % lose all
  eight lanes at the range sphere** — the rest die later, on the cone-interval
  and chord tests, which have no cheap conservative screen-space form. That caps
  any sphere-based cull (finer tiles, per-row X-intervals, the reverted per-batch
  rect cull, `FDS_CONE_SOLVE_EARLYOUT`) at ~3 % of the pass, and independently
  confirms a16567b's 6×4 → 12×8 tile result (29.2 M → 25.6 M) and round 1's
  early-out rejection.
* **Unroll-and-jam.** Ceiling ~8 % of the pass (above), and the `__m256` spelling
  already supplies 2×. Not built.
* **Outlining the cold arms.** Round 3 already measured the deletion of 10 % of
  the kernel's instructions (`-DFDS_CONE_HOTONLY=1`) at zero cycles; the mechanism
  above says why. Not built.

**Carry forward.** The remaining headroom on this pass is ~19 % of the solve
(perfect scheduling) plus whatever else can be spelled at one NEON op instead of
two. Everything that reduces instructions on the *other* pipes — spills,
branches, scalar setup — is free and will keep measuring as zero.

### Round 4b — the same metric, a second time: the solve's algebra

Once vector-ALU op count is the metric, the solve reads differently. Three
spellings cost an op they need not, and all three are provably bit-exact to fold:

* **`NEG(mul(set1(k), v))`** — three sites (`sSphD`, `sA`, `sBh`) spelled
  `a*b - c` as `fma(a, b, NEG(c))` per 7e34645's rule that pins the contraction.
  The `NEG` is a real `fneg.4s`. Broadcasting `-k` instead deletes it:
  `fl((-k)*v) == -fl(k*v)` exactly, because IEEE negation is exact and
  round-to-nearest-even is symmetric about zero, so the FMA sees the identical
  addend.
* **`mul(set1(cq), mul(sA, set1(-4.0f)))`** — folds to `mul(set1(cq*-4), sA)`.
  Both inner products are scalings by a power of two, hence exact, so either
  spelling rounds the same real product exactly once.
* **`or(mFwd, mBwd)`** — `mFwd = DV > 1e-6`, `mBwd = DV < -1e-6`, and the apex
  cut computes `mDVBig = |DV| > 1e-6` twenty lines later. Those are the same
  mask for every input, NaN (all three false) and ±0 included. Hoisting
  `mDVBig` retires a compare and an or.

18 instructions out of 4505 statically. city t=1961, parent `5b678917` vs new,
two independent interleaved min-of-6 sessions with the arm order reversed
between them: cones **0.559 / 0.557 → 0.548 / 0.543 `Gcyc/f`** (−2.0 %, −2.5 %),
wall **16.102 / 16.069 → 15.890 / 15.885 ms** (−1.3 %, −1.1 %), instructions
2.347 → 2.302 (−1.9 %). Greets held (+0.4 % cyc / −0.2 % wall, inside noise).
Bit-exact by construction and in fact: city `3cbe42b1` 3/3, greets `778fa6ac`
3/3, fountain `8db68ccb` 2/2, `render_gate` all four rows PASS.

Cumulative for round 4, measured **directly** (parent `67441d86` and the final
binary interleaved in one min-of-6 session, not composed across sessions): cones
**0.582 → 0.553 `Gcyc/f` (−5.0 %)**, **16.953 → 15.930 ms (−6.0 %)**, 2.388 →
2.302 `Ginstr/f` (−3.6 %), IPC 4.073 → 4.157. renderFrame 1.765 → 1.733
`Gcyc/f` and 57.392 → 55.976 ms; the −0.029 `Gcyc` cones saving carries the
−0.032 `Gcyc` frame saving, which is the attribution check on the counter that
is not load-sensitive. Every bit of it bit-exact — not one pin moved.

## 13. Worked example round 5 — the systematic SPELLING SWEEP, and where it dries, 2026-08-13d

Round 4 ended with a metric — **vector-ALU op count** — and one worked example
of it (`FDS_CONE_NEONMINMAX`, 19 min/max sites at two ops each, −4.5 % cycles).
Round 5 is that metric applied *systematically*: census every op the hot loop
emits, bucket it by the pipe it issues on, map each bucket back to the source
or intrinsic that produced it, and hunt every place the kernel spends more
vector ops than the operation needs.

**The sweep found two things worth landing and then dried, and the reason it
dried is the round's real result: op count is necessary but not sufficient.
What moves cycles on this pass is ops ON THE DEPENDENCY CHAIN.**

### The instruments this round adds

* **`scratchpad/cone_census.py`** — buckets a disassembled kernel into
  arith / logic / blend / cmp / dup-ins / permute / convert / reduce / mov /
  const, plus divsqrt, vector and scalar memory, scalar FP/int and branch, and
  prints the per-bucket opcode histogram. The starred buckets are the ones that
  issue on the four NEON/FP pipes; their sum is the metric.
* **`scratchpad/cone_loops.py`** — loop-nest census. Back-edges give the loop
  intervals, so the **per-spot loop** can be separated from the per-tile
  prologue and the composite. This matters: the whole-function histogram is
  4487 instructions of which 1117 are vector-ALU, but the loop that runs
  3.2 M times a frame is only part of it.
* **`scratchpad/cone_srcmap.py`** — attributes every emitted op to the
  `.loc` that produced it. Thin-LTO strips the line map from the linked binary
  and `dsymutil` cannot recover it (`llvm-dwarfdump --lookup` on the `.dSYM`
  returns nothing for this function), so the tool reads a `clang -S -g` listing
  of the TU instead — 2673 instructions against the linked control's 2709, and
  an identical bucket histogram, which is what makes the attribution usable.
  Inlined intrinsics report the *callee's* file:line, which is exactly what is
  wanted: it says which simde intrinsic emitted the op.
* **`scratchpad/cone_census_ladder.sh`** — the round-2 ablation ladder read as
  a *static* op census (`census(ABLATE=n) − census(ABLATE=n−1)`), compiled to
  assembly only, so a stage costs ~10 s instead of a full rebuild. Useful for
  orientation; the increments are contaminated by the code the earlier `continue`
  lets the compiler compress, so the loop-interval census above is the honest one.

### The census — city's hot path, single-arm control

`-DFDS_CONE_FORCE=1 -DFDS_CONE_HOTONLY=1`, the per-spot loop only:

| bucket | parent | after round 5 | what it is |
|---|--:|--:|---|
| **vector-ALU total** | **484** | **461** | the metric |
| arith (fmul/fmla/fsub/fadd/fmax/fmin/fabs/fneg…) | 246 | 236 | the actual math |
| cmp | 50 | 50 | branchless predicates |
| logic (and 32 / bic 13 / orn 2 / mvn 1) | 48 | 48 | mask algebra |
| blend (bsl 22 / bit 7 / bif 7) | 36 | 36 | selects |
| **mov (mov.16b 44)** | **49** | **42** | register copies — see below |
| dup/ins | 23 | 21 | broadcasts |
| const (movi/fmov) | 17 | 13 | in-loop constant materialisation |
| permute | 6 | 6 | the z-decode/pack, not the solve |
| int / other | 9 | 9 | |
| *(not vector-ALU)* divsqrt | 12 | 12 | 8 `fdiv.4s` + 4 `fsqrt.4s` |
| *(not vector-ALU)* vector mem | 131 | 124 | spill traffic, free on this pass |

### The `mov.16b` question, answered

139 of the shipping kernel's 4487 instructions were `mov.16b`, unexamined by
round 4. In the hot loop it is 44. They are **not** simde shuffling 128-bit
halves — simde's `__m256` is two independent `__m128` and never permutes
between them; the solve emits **zero** cross-half permutes (all 6 in the kernel
are the z-decode/pack). They are three things:

1. **Blend-operand copies.** `BSL Vd,Vn,Vm` destroys `Vd`, which holds the
   **mask**; `BIT`/`BIF` destroy a **data** operand instead. LLVM picks whichever
   operand is dead — hence the 22/7/7 split — but a select whose mask *and* both
   data operands stay live has to copy something. `mFwd` feeding two blends is
   the canonical instance: `mov.16b v7, v8` / `bsl.16b v7, …` / `bsl.16b v8, …`.
2. **FMA-accumulator copies.** `fmla Vd,Vn,Vm[i]` accumulates into `Vd`, so an
   `__m256` FMA with a **broadcast addend** needs that broadcast in *both*
   halves' destinations: one `dup` and one `mov.16b`. This is the one that
   responds to respelling, and the fold below removes two of them.
3. **Allocator/phi copies** — the `.loc`-less remainder.

### The known simde 2-for-1 suspects: CHECKED, and already clean

Every one of round 5's named suspects was verified in the disassembly rather
than assumed, and **none of them is costing an op**:

* **`_mm256_blendv_ps` → one op.** simde's `blendv` normalises the mask with a
  `cmplt` against zero first, but LLVM folds it away when the mask is already a
  compare result: **3 `cmlt.4s` survive against 36 blends**.
* **Unordered `_CMP_NLT_UQ` / `_CMP_NLE_UQ` → one op.** The NaN-correct negation
  is absorbed into `BIC`/`ORN` by the consumer. The hot loop contains **exactly
  one `mvn.16b`**. Round 3's careful unordered predicates are free.
* **`and`/`or` mask chains → already `bic`/`orn`.** No redundant `not`s.
* **`set1` broadcasts → already by-element.** The kernel emits **54 indexed
  `fmul.4s`/`fmla.4s`/`fmls.4s`** forms, i.e. LLVM already reads the scalar out
  of a lane instead of broadcasting it. The 21–23 `dup.4s` that remain feed
  `fadd`/`fsub`/`fcmp`/`fdiv`/`bsl` — **arm64 has no by-element encoding for
  those**, which is the whole reason they survive.
* **Compare-against-zero → already immediate.** 20 `fcm*.4s Vd,Vn,#0.0` forms.
* **`andnot(-0.0, x)` → `fabs.4s`**, folded.
* **Conversions**: 4 `ucvtf.4s` (the z-decode). **Horizontal reductions**:
  exactly one `faddp.2s` in the entire kernel.
* **`sqrt`/`div`**: true `fdiv.4s`/`fsqrt.4s`, deliberate — round 1 measured the
  rcp/rsqrt+NR family *slower* than exact here.

The only genuine 2-for-1 the sweep found in an *intrinsic* was the Newton step.

### Landed 1 — `FDS_CONE_NEONSTEP`: the Newton step is one instruction

arm64 has fused step instructions for exactly these recurrences:
`FRECPS = 2 − Vn·Vm` and `FRSQRTS = (3 − Vn·Vm)/2`, one rounding each. The
kernel wrote them out longhand and the shipping binary emitted **zero
`frsqrts.4s`** at three rsqrt-NR sites, spending a constant materialisation, a
broadcast copy, a multiply and an `fmls` on each. `rcp_step_x8` /
`rsqrt_step_x8` (DeferredCommon.h) route them through `vrecpsq_f32` /
`vrsqrtsq_f32`: `frsqrts.4s` 0 → 10, `frecps.4s` 10 → 12, kernel 4487 → 4454
instructions, hot loop **484 → 469 vector-ALU (−3.1 %)**.

**Bit-exact, checked over 61.4 M inputs** — every 37th representable positive
float plus 4 M log-uniform draws — **zero** bit differences for `x > 2.4e-38`.
Below that the *longhand* form is the wrong one (`0.5*x` goes subnormal and
rounds) and the native step is strictly more accurate; the kernel's arguments
never reach there.

**And it is worth <0.5 %.** Two independent interleaved sessions: instructions
reproduce exactly (2.304 → 2.269 `Ginstr/f`, −1.5 %) and wall reproduces
(−1.2 %), but cycles read 0.542 → 0.542 and then 0.548 → 0.534 — **the cycle
win does not reproduce**, so the honest label is *free*, not *a win*. Kept
because it is free, simpler and more accurate. The `NEONSTEP=0` arm is the
control that the rewrite is the only difference: 4486 instructions / 1118
vector-ALU against the parent's 4487 / 1117.

### Landed 2 — the dot product's tail is a scalar (−1.8 % cycles)

Round 4b folded three broadcast scalars in the solve's algebra; the same read
applied to the **head** of the chain finds the largest one left.
`VP = X·Px + Y·Py + Pz` and `DV = X·Dx + Y·Dy + Dz`, and **`Y·Py + Pz` and
`Y·Dy + Dz` are both per-(row × spot) scalars** — the entire tail of each dot
product is one broadcast. The port spelled it as the scalar arm does,
`add(set1(Pz), fma(set1(Px), sX, set1(Y·Py)))`, at **seven** vector-ALU ops:
`dup(YPy)`, a `mov.16b` (both halves need the FMA accumulator), two `fmla`,
`dup(Pz)`, two `fadd`. Folded, it is **four**. Two sites, −6 ops, and it
deletes an `fadd` from the head of the dependency chain.

Second fold, bit-exact by construction: `b = 2·(c2·VP − DP·DV)` was `sBh` then
`add(sBh, sBh)`; scaling by two is exact, so broadcasting `2·c2` and `−2·DP`
rounds the identical real product once and deletes the doubling add. −2 ops.

Hot loop **469 → 461**; cumulative for the round **484 → 461 (−4.8 %)**.

| cones @ city t=1961 | wall_min | `Ginstr/f` | `Gcyc/f` |
|---|--:|--:|--:|
| parent `4e643a25` | 16.139 ms | 2.304 | 0.545 |
| **round 5 (ships)** | **15.802 ms** | **2.239** | **0.535** |

**−1.8 % cycles, −2.1 % wall, −2.8 % instructions**, and the −1.8 % cycle figure
**reproduces across two independent interleaved sessions** (min-of-6 and
min-of-8) — unlike the Newton step alone. City t-sweep, min-of-4 per pose:
t=400 −3.5 % cyc / −6.0 % wall, t=900 −1.8 % / −2.3 %, t=1400 −0.9 % / −2.2 %,
t=2400 −3.0 % / −3.0 %. **Greets** −2.1 % cyc / +0.4 % wall (its narrow cones
take the segmented path, so only the Newton step reaches it).

**Bytes — the judge call, with the numbers.** The VP/DV fold is *not*
bit-exact: the scalar arm rounds `Y·Py` before adding `Pz`, this rounds the
fused `Y·Py + Pz` once. Measured across the city sweep:

| pose | differing px of 2 073 600 | max \|Δ\| |
|---|--:|--:|
| t=400 | 3 (0.0001 %) | 2/255 |
| t=900 | **byte-identical** | — |
| t=1400 | **byte-identical** | — |
| t=1961 | 2 (0.0001 %) | 1/255 |
| t=2400 | **byte-identical** | — |

Z-buffer identical at every pose; greets and fountain byte-identical. The two
t=1961 pixels are `(1852,429) (150,93,87)→(151,94,87)` and
`(1875,435) (104,89,209)→(103,89,209)`. Artifact battery clean: the pixels are
**isolated**, so there is no striping or banding to have; a NaN is excluded by
construction at max |Δ| = 2 LSB (a NaN blows out a whole 8-lane batch, it cannot
show as two pixels one level apart); and there is no temporal structure to
shimmer when three of five poses are byte-identical. `render_gate` **ALL FOUR
rows PASS**, including `conetest b41894f9` — direct coverage of this kernel,
**bit-identical**. Images: `docs/img/conevec/r5_city_t1961_crop_before_after.png`,
`docs/img/conevec/r5_city_t1961_diff.png`, `docs/img/conevec/r5_city_t400_diff.png`.

**Pin moved, documented:** city `3cbe42b166847e40f7071eedb48d613c` →
`3f8948232c192a979ffe7f76c4b387ab` (2/2 stable). greets `778fa6ac…` and
fountain `8db68ccb…` unmoved.

### Killed, with numbers

* **Hoisting `1/uV` out of the spot loop.** `uV = X²+Y²+1` is a property of the
  8-pixel batch, and the solve appears to divide by it once per (batch × spot).
  **It does not: LLVM's LICM already hoists it.** The parent's control build
  divides at batch level (`0x1001e7edc`, right after the `spotCount<=0` guard),
  spills the result and reloads it inside the loop. Written, built, and the
  disassembly is unchanged — 10 `fdiv.4s` before and after, same placement.
  Killed on the census, never benched.
* **Collapsing the `zLo`/`zHi` select cascade.** `zLoP` re-selects on `mDisc`
  what `zLoRt` just selected on `mFwd` against the same fallback, so the two
  collapse into one blend on `mDisc & mFwd` (and `mDisc & ~mFwd` for `zHi`) —
  bitwise identical, and it takes `zLo`/`zHi` off the discriminant sqrt through
  **two** serial selects instead of three. This was built as a deliberate test
  of the round's hypothesis, because it trades op count *against* chain length:
  the hot loop went **461 → 469 vector-ALU (+8)** — the two new masks cost two
  `and`/`bic` and, via register pressure, **four more `mov.16b`** — while the
  chain got shorter. Measured, interleaved min-of-6, city t=1961: **+0.6 %
  cycles, +1.2 % wall, +0.6 % instructions.** Reverted.

### THE FINDING: op count is necessary, not sufficient — it has to be ON THE CHAIN

Line the round's four results up against the two candidate models:

| change | Δ vector-ALU ops | on the dependency chain? | Δ cycles |
|---|--:|---|--:|
| round 4 `NEONMINMAX` | −2/site × 19 sites | **yes** — cmp+blend is a 2-deep serial pair, `fmin` is 1 | **−4.5 %** |
| round 4b algebra folds | −18 static | **yes** — `fneg`/`mul` sat between the FMAs | **−2.0 %** |
| round 5 VP/DV fold | −8 (−1.7 %) | **yes** — deletes an `fadd` at the head of the solve | **−1.8 %** |
| round 5 Newton step | −15 (−3.1 %) | **no** — constants, copies; step depth is unchanged either way | **~0** |
| round 5 select collapse | **+8 (+1.7 %)** | shortens it by one link | **+0.6 %** |

**The two changes that moved the most cycles removed the FEWEST ops.** Round 4's
"81 % of the NEON issue ceiling" is real but it is not the whole constraint:
the solve's long pole is `fma → fma → fma → fsqrt → fsub → fmul → fmin/fmax →
bsl → bsl → bsl → fmax → fmax`, roughly thirteen links with a ~12-cycle
`fsqrt` in the middle, which accounts for most of the measured ~63 cycles per
(batch × spot) on its own. Ops that dual-issue into the slack around that chain
are free to remove **and free to keep** — which is round 3's finding
("deleting 7–10 % of instructions moved cycles by zero") reappearing one level
down, now with a mechanism.

And the last row is the sting: **shortening the chain does not automatically pay
either**, because the register file is the thing that pushes back — two extra
live masks bought four `mov.16b`, and the copies cost more than the link saved.

> **Carry forward — this retires the spelling direction.** The named simde
> 2-for-1 suspects are all verified clean; the one real find (the Newton step)
> measured free. What is left in the hot loop is 236 arith ops of actual math,
> 134 ops of branchless mask/select control flow (29 % — that IS the 8-wide
> algorithm), and 76 ops of copies, broadcasts and constants that are all
> off-chain by construction and therefore, by this round's evidence, worth
> approximately nothing. **The next win on this pass is not a spelling. It has
> to shorten the solve's dependency chain without adding live values, or do
> less work — and culling is already closed at ~3 % (round 4).**

---

## 14. Worked example round 6 — the campaign profiled the wrong scene, 2026-08-14

Rounds 1–5 all measured **city**. Round 1's frame-time rebaseline
(`d9742245`, PERF_STATE §00) then found that the biggest cone bill in the demo
is **chase**: `Render_VolumetricCones_Tile` is **46.6 % of chase's entire CPU
self-time**, 21.6 ms of a 46.9 ms `renderFrame`, 2.817 Ginstr/f — and chase had
never been in the campaign. Round 6 is that scene's first pass, and the first
thing it establishes is that chase's cones **are not the same code** as city's.

### 14.1 The census, and why it changes the whole answer

`-DFDS_CONE_DIAG=1`, run through the snapshot harness (chase has no `--bench`
arm). Two counters were added this round: `quaddead`, below, and the scalar
arm's share of the picture.

| pose | tile-entries | (batch × spot) | **seg %** | dead % | alive lanes/pair | **sphdead** | **quaddead** |
|---|--:|--:|--:|--:|--:|--:|--:|
| chase t=100 | 1238 | 3 342 600 | **100.0** | 94.3 | 0.45 (5.6 %) | 0.2 % | **86.2 %** |
| chase t=400 | 1334 | 3 601 800 | **100.0** | 94.6 | 0.42 (5.2 %) | 0.0 % | **89.4 %** |
| chase t=800 | 832 | 2 246 400 | **90.9** | 91.9 | 0.63 (7.9 %) | 0.0 % | **83.9 %** |
| chase t=1200 | 555 | 1 498 500 | 65.6 | 69.0 | 2.46 (30.8 %) | 0.0 % | 63.1 % |
| chase t=1600 | 96 | 259 200 | 0.0 | 100.0 | 0.00 | 3.4 % | 3.4 % |
| city t=1961 | — | 3 204 900 | 0.0 | 39.9 | 4.78 (59.8 %) | 7.7 % | 18.8 % |
| greets t=1588 | — | 2 044 | 100.0 | 80.7 | 1.47 (18.4 %) | 0.0 % | 70.6 % |

Chase carries **34 spots of which 32 are narrow** (`cosOuter > 0.985`), so
90–100 % of its (batch × spot) pairs take the **8-segment hybrid** — greets'
branch, not city's. None of them casts (`shadowed=0`), and there are no
clone/bounce spots (`CONE-ATTR real=34 bounce=0 clone=0`). The city rows
reproduce `a16567b`'s numbers exactly (39.9 % dead, 7.7 % sphdead), which is
the instrument's own control.

### 14.2 The ladder, re-run on chase — the solve is back to being the pass

Round 2's `-DFDS_CONE_ABLATE=n` ladder, driven through the snapshot harness by
the new `scratchpad/chase_ablate.sh`. chase t=800, 8 snapshots, min over runs
with run 1 discarded:

| what is KEPT | Ginstr/f | Gcyc/f |
|---|--:|--:|
| per-batch floor (no spot loop) | 0.105 (3.8 %) | 0.023 (3.7 %) |
| + per-spot scalar prologue | 0.233 (8.3 %) | 0.045 (7.2 %) |
| **+ the cone-interval SOLVE** | **1.487 (53.2 %)** | **0.335 (53.7 %)** |
| + scalar per-lane dz/fade loop | 0.075 (2.7 %) | 0.014 (2.2 %) |
| + the whole integration BODY | 0.866 (31.0 %) | 0.184 (29.5 %) |
| + per-lane colour accumulate | 0.031 (1.1 %) | 0.023 (3.7 %) |
| **total** | **2.797** | **0.624** |

City's solve, after the round-1 port, is 34.6 % of its pass. Chase's is
**53.2 %** — because on chase it was **still the scalar loop**. The stage 6–10
sub-ladder inside the body came out non-monotonic again, exactly as round 3
warns; it is not quoted.

### 14.3 What was gated off, and why the gate was right then and wrong now

`--vol_cone_solve_vec` — the round-1 headline, −30 % of city's pass — carried
`&& !segPath` in its gate. That term was set on a **greets** measurement:
ungated at the greets pin pose the wide arm read instructions −3.8 % but
**cycles +3.6 % and wall +1.4 %**, because the wide arm computes both a-sign
branches and the whole tail for all eight lanes where the scalar arm's dead
lanes bail early.

That gate put 91 % of chase's pairs on the scalar arm. **Removing the term is
worth −11.8 % instructions on chase's pass on its own** — and it now *improves*
greets too. What changed is the kernel, not the argument: `NEONMINMAX`, the
round-4b and round-5 algebra folds and the `lane_vec` loops all landed after
the gate was set.

**The gate must be removed, not flagged.** A three-arm run priced both shapes:

| chase t=800, cones | wall_min | Ginstr/f | Gcyc/f |
|---|--:|--:|--:|
| parent | 19.991 | 2.800 | 0.672 |
| new FeatureFlag, ON | 19.780 | 2.615 | 0.657 |
| `!segPath` term simply deleted | 19.542 | **2.470** | **0.644** |

The flagged arm is **+5.9 % instructions** against the unconditional one — round
3's dual-arm tax, at the size round 3 measured it. One extra live bool in this
kernel costs more than most of the wins the campaign has landed.

### 14.4 `FDS_CONE_QUADEARLYOUT` — round 1's early-out at the right cut point

Round 1 built a range-sphere early-out (`FDS_CONE_SOLVE_EARLYOUT`) and it cost
**+2.0 %** on city: a pair is only skipped when all EIGHT lanes miss the range
**sphere**, and city loses only 7.7 % of its pairs there.

**Chase loses 0.0 % there.** Its spots are long-range beams whose range sphere
covers the whole visible depth — the sphere never rejects, and every dead lane
dies on the **cone quadratic** instead, thirty lines further down. Moving the
cut to just after the a-sign/discriminant mask — the first point at which the
pair's fate is decided — takes the fire rate from 0.0 % to **83.9 %** and skips
a `sqrt`, two divides, ~22 vector ops and three stores per pair.

It is **byte-null by construction**: `zLoArr`/`zHiArr`/`aliveLane` are
zero-initialised per pair and `spotAlive` stays false, which is exactly the
state the fall-through leaves them in before `if (!spotAlive) continue;`.

**And the fire rate is the whole story.** 83.9 % (chase t=800) is a large win;
18.8 % (city) is break-even on instructions and slightly positive on cycles;
**3.4 % (chase t=1600) is a 4 % LOSS** — the same shape as round 1's rejection,
reproduced from the other end. This is the number to check before proposing any
future cull on this pass.

### 14.5 Measured, three arms, parent binary, interleaved min-of-6

`scratchpad/prof1.py`, per-arm binaries, one asset tree, arm order rotating
every round, round 0 discarded.

| chase t=800 | cones wall | cones Ginstr/f | cones Gcyc/f | IPC | renderFrame wall |
|---|--:|--:|--:|--:|--:|
| parent `43ac3456` | 18.304 | 2.796 | 0.622 | 4.494 | 40.608 |
| + solve ungated | 17.783 | 2.467 | 0.595 | 4.146 | 39.394 |
| **+ quad early-out (SHIPS)** | **14.930** | **2.192** | **0.500** | 4.380 | **36.723** |
| | **−18.4 %** | **−21.6 %** | **−19.6 %** | | **−9.6 %** |

**Attribution check**: the frame's instruction saving is −0.606 G and the cone
pass's is −0.604 G; the frame's cycle saving is −0.125 G against the pass's
−0.122 G. The saving is where the timer says it is.

**chase t-sweep** (parent → ships):

| pose | cones wall | cones Ginstr/f | cones Gcyc/f | renderFrame wall |
|---|--:|--:|--:|--:|
| t=100 | 23.917 → 18.854 (**−21.2 %**) | 3.748 → 2.844 | 0.847 → 0.676 | 43.687 → 39.014 (−10.7 %) |
| t=400 | 25.070 → 20.166 (**−19.6 %**) | 3.927 → 3.014 | 0.869 → 0.698 | 43.818 → 38.724 (−11.6 %) |
| t=800 | 18.304 → 14.930 (**−18.4 %**) | 2.796 → 2.192 | 0.622 → 0.500 | 40.608 → 36.723 (−9.6 %) |
| t=1200 | 13.167 → 11.923 (−9.4 %) | 1.998 → 1.778 | 0.467 → 0.415 | 39.092 → 37.410 (−4.3 %) |
| t=1600 | 3.067 → 3.193 (**+4.1 %**) | 0.487 → 0.501 | 0.105 → 0.112 | 19.418 → 19.523 (+0.5 %) |

t=1600 is the pose with **zero** segmented pairs and a 3.4 % early-out fire
rate — nothing to win and a branch to pay. Its cone pass is 3.1 ms of a 19.4 ms
frame, so the whole regression is +0.1 ms.

**The other two cone scenes, same protocol:**

| | cones wall | cones Ginstr/f | cones Gcyc/f |
|---|--:|--:|--:|
| greets t=1588 (pin pose) | 7.290 → **6.582** (−9.7 %) | 1.122 → 0.994 | 0.240 → 0.217 (−9.6 %) |
| greets t=3122 (the user's pose) | 6.055 → 6.016 (−0.6 %) | 0.900 → 0.893 | 0.204 → 0.201 (−1.5 %) |
| city t=1961 | 15.677 → 15.535 (−0.9 %) | 2.240 → 2.266 (+1.2 %) | 0.540 → 0.537 (−0.6 %) |

**greets — the scene the gate was set to protect — improves at both poses.**
City is all-wide, so the change reaches it only as codegen: instructions +1.2 %,
cycles −0.6 %, wall −0.9 %. Per round 3, cycles is the column to believe on this
pass, and city's cycles do not regress. The early-out is what keeps it whole:
without it, city read +2.0 % cycles.

### 14.6 The byte verdict — a judge call, and a small one

Not byte-null, and the reason is inherited: the wide arm's VP/DV fold (round 5)
rounds `Y·Py + Pz` once where the scalar arm rounds `Y·Py` first, so the
segmented cones now take round 5's judge call too.

| pin | before | after | px changed | max \|Δ\| |
|---|---|---|--:|--:|
| chase t=100 | `76e7cf68…` | `7678a6bc6ea964b3b859ecb11c0673c3` | 20 of 2 073 600 (0.0010 %) | **2/255** |
| chase t=400 | `d458e82b…` | `42d79fadd825a329b36143efe052edfb` | 27 (0.0013 %) | **2/255** |
| chase t=800 | `c145c7a5…` | `b29c73f1c54f42a02e0dc2484780cc03` | 40 (0.0019 %) | **2/255** |
| chase t=1200 | `31aa5203…` | **unmoved** | 0 | 0 |
| chase t=1600 | `1544b0e7…` | **unmoved** | 0 | 0 |
| greets t=1588 | `778fa6ac…` | `570a7b443f768393dc6647044a9e67b3` | 381 (0.0184 %) | **1/255** |
| city t=1961 | `3f894823…` | **unmoved** | 0 | 0 |
| fountain t=2500 | `8db68ccb…` | **unmoved** | 0 | 0 |

Not one pixel anywhere moves more than 2/255, and none moves more than 4/255 on
any channel. `render_gate` all four rows PASS, `conetest b41894f9…`
byte-identical (its cones are wide). Images:
`docs/img/chasecone/{chase_t800,chase_t400,chase_t100,greets_t1588}_{where,sbs}.png`.

### 14.7 Carry forward

* **The per-spot scalar prologue is 8.3 % of chase's pass and 7.2 % of its
  cycles** — 104 instructions per (batch × spot), including a **divide**
  (`1/(cosI − cosO)`), for values that are per-SPOT invariants recomputed for
  every batch because the spot loop is innermost. A per-tile precompute of
  `DP`, `PP`, `c2`, `invCIO` is bit-exact by construction and unattacked.
* **greets carries `shadowed=51` of 51 spots** — every greets beam casts, so its
  segmented body pays the per-segment scalar shadow tap (8 taps × alive lanes)
  that chase pays nothing for. That is a greets-only lever nobody has priced.
* The 8-segment loop recomputes `W2` and `D·W` from `Wx/Wy/Wz` per segment when
  both are available in closed form from `uV`, `VP`, `DV` and `PP` — values the
  solve already produced. ~8 vector ops × 8 segments per alive pair. It is a
  re-association, so it is a judge call, not a transcript.
* **Do not price a cull on this pass without its fire rate first.** 14.4 has the
  same instrument giving 83.9 %, 18.8 % and 3.4 % on three poses of two scenes,
  and the verdict flips sign across that range.

## 15. Worked example round 7 — the innermost loop was rebuilding per-LIGHT constants, 2026-08-15

§14.7 parked one item as *"bit-exact by construction and unattacked"*: the
per-spot scalar prologue — **8.3 % of chase's cone pass and 7.2 % of its
cycles, 104 instructions per (batch × spot) including a divide**. This is that
item. It is the cheapest thing the campaign has landed, because it is not an
algorithm change at all: the same expressions, on the same operands, evaluated
once instead of ten thousand times per tile.

### 15.1 The inventory — what in that loop actually depends on the batch

The kernel's nest is **row → 8-px batch → spot**, with the spot loop
**innermost**. Read against that nest the whole prologue is loop-invariant:

| value | varies with | verdict |
|---|---|---|
| `Px,Py,Pz` `Dx,Dy,Dz` `cosO,cosI` `r2,rr` | light index | **per-spot** (12 scattered SoA loads) |
| `narrowCone`, `nSamp`, `inv_nSamp`, `segPath` | `cosO`, `turb.on` | **per-spot** (4 selects) |
| `omid`, `bounce` + their two `continue`s | light index | **per-spot** |
| `hsNx/hsNy/hsNz/hsD` (bounce half-space) | light × `viewToWorld`, camera | **per-frame** |
| `DP = D·P`, `PP = P·P`, `c2 = cosO²` | the above | **per-spot** (2 three-term dots) |
| `inv_cosI_minus_cosO` | `cosI`, `cosO` | **per-spot — THE DIVIDE** |
| `Y·Py`, `Y·Dy` (`VPk`/`DVk`) | the row's `Y` | per-**(row × spot)** — *not* hoisted, see 15.6 |
| `X`, `uV`, `zMax`, `pxHash` | the pixel | genuinely per-batch |
| `VP`, `DV`, `a`, `b`, `disc`, the roots | the pixel | genuinely per-batch |

And one stage further down, *inside* the solve, five more per-spot values the
8-wide arm was rebuilding for every pair: `sphereC = PP − r2`,
`cq = fma(DP,DP,−(c2·PP))`, and the three broadcast constants `c2+c2`,
`−(DP+DP)`, `cq·−4`. The last three are **exact** operations (power-of-two
scaling and negation), so folding them rounds nothing that was not already
rounded.

**What was deliberately left alone**, and why it is worth saying: the
shadow-map block (`smIdx`, `smMirror`, the mirror plane, the 17 `sm_*`
scalars) is *also* per-spot invariant, but it sits **after** `if (!spotAlive)
continue;` — it runs on 8.1 % of chase's pairs, and hoisting it converts loads
into loads rather than deleting arithmetic. Left for a measurement, not
assumed.

### 15.2 The hoisting scope — per TILE, because that is where the spot list is

`Render_VolumetricCones_Tile` receives `spotIdx/spotCount`: the **per-tile**
list `43ac3456`'s sphere cull builds. So the smallest scope that makes these
compute-once without touching a signature is *per tile, per spot*, in a
`ConeSpotPre[64]` record array built between the `yStep` line and the row
loop. The amortisation is not marginal: a coarse 6×4 tile at 1920×1080 is
320×270 px = **40 batches × 270 rows = 10 800** evaluations collapsed to one.
Going per-*frame* would save the remaining 0.01 % and would need the record
threaded through the dispatcher — not worth it.

Two details make the compacted list exact rather than merely equivalent:
the prologue's two `continue`s (clone spot with no mirror mask; bounce spot
with the camera on the glass) are hoisted **into the precompute as
non-appends**, and both sat *before* the first `g_coneDiag` counter, so the
census is unaffected too.

### 15.3 Bit-exact, verified rather than argued

Every field is a **verbatim move** of the line it replaces — no term
re-associated, no product respelled — so the compiler's contraction map (§13)
travels with the value. The check is the pin battery, and it passed first try:

* **chase** t100 `7678a6bc…` t400 `42d79fad…` t800 `b29c73f1…` t1200
  `31aa5203…` t1600 `1544b0e7…` — **all five unmoved**
* **greets** `570a7b44…`, **city** `3f894823…`, **fountain** `8db68ccb…` —
  unmoved
* 2/2 stable on each binary, **differential** (both binaries built in one
  worktree, one asset tree, run 1 discarded)
* `render_gate` **ALL FOUR rows PASS** byte-identical

**Not one pixel moves anywhere.** That is what "bit-exact by construction"
is supposed to buy, and it is the reason this change needs no judge call.

### 15.4 Measured — two arms, parent binary, interleaved min-of-6

No FeatureFlag: §14.3 priced the dual-arm tax in this kernel at **+5.9 %
instructions** for one extra live bool, so a hoist that is bit-exact ships as
a straight replacement. Two independent interleaved sessions, `scratchpad/prof1.py`,
per-arm binaries, one asset tree, order rotating each round, round 0 discarded.
Load 5–22 — read `Ginstr/f`, which reproduced **to 0.15 % between the two
sessions** for every row.

| pose | cones wall | cones Ginstr/f | cones Gcyc/f |
|---|--:|--:|--:|
| chase t=800 | 14.766 → **13.601** (−7.9 %) | 2.191 → **1.992 (−9.1 %)** | 0.502 → 0.464 (−7.6 %) |
| chase t=400 | 20.114 → **18.527** (−7.9 %) | 3.013 → **2.691 (−10.7 %)** | 0.688 → 0.625 (−9.2 %) |
| city t=1961 | 15.529 → 14.998 (−3.4 %) | 2.264 → **2.081 (−8.1 %)** | 0.540 → 0.520 (−3.7 %) |
| greets t=1588 | 6.545 → 6.270 (−4.2 %) | 0.994 → **0.951 (−4.3 %)** | 0.220 → 0.207 (−5.9 %) |
| greets t=3122 (the user's pose) | 6.186 → 5.997 (−3.1 %) | 0.893 → 0.889 (−0.4 %) | 0.212 → 0.206 (−2.8 %) |

**Attribution check** (frame delta vs pass delta, Ginstr/f): chase t=800
−0.199 / −0.199; chase t=400 −0.320 / −0.322; city −0.181 / −0.183; greets
t=1588 −0.042 / −0.043. The saving is where the timer says it is.

**It beats its own price, and the reason is honest bookkeeping**: §14.7 priced
the prologue at 8.3 % and chase measures −9.1 %, because the five per-spot
values lifted out of the *solve* (15.1) are not in that 8.3 % bucket.

**This is the first change of the campaign that helps all three cone scenes**,
and the mechanism says why: the prologue runs for **every** (batch × spot)
pair regardless of which branch the pair then takes, so city's all-wide cones
pay it exactly as chase's narrow ones do. Round 6 could only reach city as
codegen; this reaches it directly (−8.1 % instructions on 3.2 M pairs).

### 15.5 The other parked item, measured and NOT kept

§14.7's third bullet: the 8-segment loop rebuilds `W = z·V − P` per segment
when both dot products are closed forms in `z` from values the solve already
produced — `W·W = z(z·uV − 2VP) + PP`, `D·W = z·DV − DP`. **3 vector ops per
segment against 11**, ×8 segments. Built and measured (`FDS_CONE_SEG_CLOSEDFORM`,
compiled out in place as the record):

| pose | cones Ginstr/f | cones Gcyc/f | IPC |
|---|--:|--:|--:|
| chase t=800 | 1.993 → 1.995 (**+0.1 %**) | 0.463 → 0.455 (−1.7 %) | 4.30 → 4.39 |
| chase t=400 | 2.690 → 2.696 (**+0.2 %**) | 0.626 → 0.616 (−1.6 %) | 4.30 → 4.38 |
| greets t=1588 | 0.952 → 0.959 (**+0.7 %**) | 0.213 → 0.207 (−2.8 %) | 4.48 → 4.62 |

**The instruction column is a small LOSS on all three, and the arithmetic says
why: the segment loop runs only on ALIVE pairs.** Chase t=800 is 91.9 % dead
(§14.1), so 8.1 % × 3 601 800 pairs × 64 saved `__m256` ops is **~0.9 % of the
pass GROSS** — before the closed form's own per-pair setup and its effect on
register allocation eat it. What survives is a shorter dependency chain, worth
1.6–2.8 % of cycles.

**Not kept**, and the rule it follows is §14.7's own: under the 2 % bar, and
it is a **re-association**, so buying it means moving bytes. Priced that too,
so the next person does not have to: chase **75 / 85 px** of 2 073 600 at max
|Δ| **2/255**, greets **2 323 px (0.112 %)** at 2/255. A judge call, an
artifact battery and a countersign for −1.7 % of one pass's cycles is not a
trade worth making.

**The reusable form of this**: *an optimisation inside a branch is worth its
op count times the branch's FIRE RATE, not its op count.* §14.4 said the same
thing about culls from the other direction. On this pass the census
(`-DFDS_CONE_DIAG=1`) prints that rate; read it before building the arm.

### 15.6 Carry forward

* `Y·Py` and `Y·Dy` are per-**(row × spot)** and still recomputed per batch.
  Hoisting them needs a per-row array refreshed over the spot list — 2 fma
  saved per pair against a 128-float write per row. Priced at ~0.15 % of the
  pass by op count; almost certainly under the noise floor, and stated here so
  nobody re-derives it.
* The per-spot **shadow-map block** (15.1) is the same shape of hoist one
  stage down, gated behind `spotAlive`. It converts loads to loads, so its
  value is the branch structure, not the arithmetic — measure before building.
* **greets still carries `shadowed=51` of 51 spots** and pays a per-segment
  scalar shadow tap chase pays nothing for. Unpriced, unchanged from §14.7.
* Chase's cone pass has now gone **2.817 → 2.192 → 1.992 Ginstr/f** across
  rounds 6 and 7, −29.3 % in total, on a scene the campaign had never opened
  two days ago.
