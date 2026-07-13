# OPTIMIZATION BACKLOG

Tracked list of deferred optimizations + measured-quality upgrades so they
don't get lost. Rule for this CPU software renderer (measured): it is
**gather-bound**, not FLOP-bound — but "texture reads are expensive" is NOT a
safe assumption (the env-reflection tap measured ~free). So: **measure each
change** (bench `--snapshot=<scene>@t=<T>@iters=<N>` → `mean ms/iter`,
interleave flag off/on ≥6×, take mins vs the noise floor). Everything here is
behind a default-off flag until measured + look-approved.

Status keys: TODO · IN-PROGRESS · DONE · PARKED (measured not-worth / blocked).

## PBR quality series (incremental, one at a time, user reviews each)
Discussion: engine PBR compositing vs canonical (SESSION_STATE / chat 2026-07-13).
The engine is standard in the big decisions (additive diffuse+specular, GGX,
Schlick, metalness→F0, roughness-mip reflection); it simplifies in 3 places —
each is a candidate below.

- **#1 analytic split-sum env-BRDF** — DONE (e0640fe, flag `env_brdf_analytic`,
  default OFF). Measured +0.72 ms on greets = within noise → effectively free.
  Retires the `f90=1-rough` hack. Awaiting user look-approval to default ON.
- **#2 SH irradiance ambient** — TODO (the big look win). Replace the flat
  `Ambient` constant with 9-coeff L2 SH evaluated per-pixel along the normal
  (~9 FMAs, no gather). Project the scene's sky-cube → 27 floats at bake. Gives
  directional colored fill (sky tint from above); ties the chase sky-cube into
  actual scene lighting — directly attacks "dark/flat". Measure per-pixel eval
  cost (hits every lit pixel, not just reflective). Highest quality-per-cost.
- **#3 (1−F) diffuse energy conservation** — TODO. Multiply diffuse `fd` by
  `(1-fres)` at the combine (fres already computed). ~1 op, fixes grazing-edge
  over-brightness. Trivial.
- **#4 multi-scatter compensation** (Fdez-Agüera) — TODO, after #1. A few ALU
  ops using the env-BRDF terms; rough metals stop reading too dark. Free add-on.
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

## Correctness/determinism (blocks the gate, not just perf)
- **Greets env-bake non-determinism** — TODO, now URGENT-ish. Greets renders
  run-to-run distinct (4/4 by 2026-07-14, up from the old "~1-in-12 flip") —
  amplified by the user's new metallic materials (more env probes; env-bakes
  vary). This BREAKS the greets md5 gate (gate on city/fountain/pbrtest until
  fixed). Root-cause the bake ordering/threading nondeterminism. Related:
  measurement-tool-traps memory.
- **Editor metallic-import OOM** — IN-PROGRESS (targeted per-surface re-bake +
  capped editor bake res). Same env-probe subsystem as the determinism issue.

## How this list is maintained
Add an entry the moment an optimization is deferred (with: what, why deferred,
where in code, expected cost/benefit). Mark DONE with the commit + measured
result. SESSION_STATE "Queued next" points here; the memory
`optimization-backlog` points here too.
