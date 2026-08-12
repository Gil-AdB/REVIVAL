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

### The lever that is left, and its measured ceiling

Vectorize the per-lane quadratic solve to 8 wide. It is the only lever with real
headroom: **2.681 G of 4.217 G instructions**, currently at 1 lane per iteration
feeding a body that is already 8-wide. The standing comment in the source
(*"the a-sign branching is too hairy to vectorize cleanly"*) is the reason it was
never done, and the three-way sign branch on `a` is genuinely the hard part —
but both live arms compute the same `disc`/`sqrt`/`1/(2a)` shape and differ only
in which root interval they take, which is a blend.

**The risk that decides it is bit-exactness, not correctness.** The city pin
`3cbe42b166847e40f7071eedb48d613c` is a byte gate, and `-ffp-contract` means the
scalar arm's `Dx*X + Dy*Y + Dz` may already be an FMA; an 8-wide port has to
reproduce the compiler's contraction choices per lane or the pin moves. Use
`_mm256_sqrt_ps` / `_mm256_div_ps` (IEEE-exact, unlike the `rsqrt`/`rcp`+NR
approximations used elsewhere in this file) and verify against the pin, not
against a screenshot.
