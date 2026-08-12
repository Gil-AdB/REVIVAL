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

### 1a. Instruments / xctrace — reachable, but NOT via `xcrun`

Xcode 26.6 is installed in `/Applications`. The 2026-08-10 row above was correct
*when written* and is now stale in its conclusion but still correct in its
diagnosis: **`xcrun xctrace` fails**, because `xcode-select -p` still points at
`/Library/Developer/CommandLineTools`, and flipping it needs `sudo`. `xcrun`
resolves tools only inside the *selected* developer dir, and `xctrace` ships
only inside `Xcode.app` — so `xcrun` can never find it while CLT is selected.

Call it by absolute path instead. No sudo, no `xcode-select` change:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
X=/Applications/Xcode.app/Contents/Developer/usr/bin/xctrace
$X version          # -> xctrace version 16.0 (17F113)
$X list templates   # Time Profiler, CPU Counters, System Trace, Allocations, ...
```

**Recording a headless bench** (`--env` per variable; the target must exit or
hit `--time-limit`, and traces belong in a scratch dir, never the repo):

```sh
cd Runtime
$X record --template 'Time Profiler' --output /tmp/greets_tp.trace \
   --time-limit 25s --no-prompt --target-stdout /dev/null \
   --env SDL_VIDEODRIVER=dummy --env SDL_AUDIODRIVER=dummy \
   --launch -- $PWD/DEMO --bench=scene@scene=greets,t=5743,iters=200 \
        --deferred --texture_filter=1 --profiler=1 --strict_flags
```

**Reading it back without the GUI.** `--toc` lists the tables; the one worth
having is `time-profile`. Export it to XML and aggregate:

```sh
$X export --input /tmp/greets_tp.trace --toc                     # find the table
$X export --input /tmp/greets_tp.trace --output /tmp/tp.xml \
   --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'
```

Two traps in that XML, both of which silently yield an EMPTY profile:

1. The stack is under `<tagged-backtrace><backtrace><frame>`, **not** `<row>
   <backtrace>`. A parser that looks for `backtrace` directly under `row`
   finds nothing and reports zero samples for every symbol.
2. Everything is id/ref deduplicated — `<frame ref="165"/>`,
   `<tagged-backtrace ref="227"/>`. You must cache `id -> value` on first sight
   and resolve `ref` against that cache, or most rows resolve to `?`.

Filter by `sample-time` to drop process init (asset load, PNG decode, mip
generation) — on a greets bench that is the first ~12 s and it will otherwise
dominate the leaf table with `stbi__do_zlib` and `MipmapXY`.

**What this buys over `/usr/bin/sample`:** real per-symbol *self* vs *inclusive*
attribution over ~78 k steady-state samples rather than a handful of stack
snapshots, so a leaf worth 3 % of the process is visible and trustworthy. It
does **not** lift the root-only PMU restriction — the CPU Counters template can
select events, but the counters this repo can read per phase are still the two
in §1. Cross-read Instruments for *which symbol*, `--hw_prof` for *how many
instructions*.

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
