# GpuBench greets frame-time sweep — chasing the reported 85–90 fps dips

Question asked: the GPU window occasionally drops to **85–90 fps (11.1–11.8 ms/frame)**.
Is that GPU pass cost, or something in the window loop?

> ## CORRECTION (2026-08-09) — read this before the sections below
>
> The original answer ("not reproducible offscreen, therefore the window path")
> was **wrong, and wrong for a boring reason: the sweep used the wrong tess
> knob.** It ran `--tess_px=8`. The user's window session that produced the dip
> ran `../build-gpu/GpuBench/GpuBench --window --tess --tess_px=1 --tess_presplit=8`
> — **`--tess_px=1`**, pixel-level tessellation, which the sweep never sampled.
> His HUD at t=2841 (SPLINE, paused) read `GPU FRAME 9.680 MS`.
>
> Re-measured, everything below the line is at 1920×1080 on the authored spline,
> `--reanimate`, **min of 3 interleaved reps of the median of 100 frames** (the
> machine was carrying two other jobs at the time — see "Measurement hygiene"):
>
> | what | measured |
> |---|---|
> | t=2841, `--tess_px=8` (the sweep's knob) | **5.24 ms** |
> | t=2841, `--tess_px=1` (his knob) | **8.35 ms** |
> | worst pose found at `--tess_px=1`, t=2500 | **8.24 ms** |
> | worst pose at `--tess_px=8` over the same grid | **5.74 ms** |
>
> So **"poses over 8 ms: 0" is a `--tess_px=8` artifact.** At px=1 the same
> timeline clears 8 ms. The dip *is* substantially GPU pass cost.
>
> **Three things it is NOT, each measured, not argued:**
>
> 1. **It is not the Retina drawable.** The window renders the scene at
>    `scene.xres × scene.yres` (1920×1080 by default) and *blits* that to the
>    drawable — `Deferred.mm:322` `const int W = scene.xres, H = scene.yres;`,
>    nothing assigns `opt.xres` from the window, and there is no
>    `SDL_WINDOWEVENT` resize handler at all. Only the final blit and the HUD
>    touch drawable-resolution pixels. (For scale: forcing the *offscreen*
>    render to 2560×1440 at px=1 costs **11.78 ms** — i.e. if the scene render
>    ever were wired to the drawable, it would land squarely in the 85–90 fps
>    band. It is not wired today.)
> 2. **It is not second-order mirrors.** At the heaviest pose and his exact tess
>    knob, `--mirror2` on vs off is **8.315 → 8.247 ms, +0.07 ms**. The +0.11 ms
>    from d055f9e holds. The HUD's `MIRROR2 6.694` is an *overlapping encoder
>    span*, not a cost — see the boxed warning under "Result".
> 3. **It is not tessellation leaking into the reflection passes.** The mirror
>    and env/shadow bakes take the *flat* stone by construction —
>    `Deferred.mm:3018` `tessThisPass = false;  // mirrors + bakes take the flat
>    stone`, which is CPU parity (`greets_displace_flat_mirror`,
>    `GREETS.CPP:1140-1142`). Corroborated by measurement: the `mirror` span
>    barely moves between px=1 and px=8 (2.13 vs 2.39 ms at t=2841). So
>    `--tess_px` does not multiply through the mirrors, and a
>    "coarser tess for reflections" knob would buy nothing.
>
> **Additive attribution at t=2841, px=1/P=8, 1920×1080** (each row is a
> frame-total delta from toggling one thing, which is the only legitimate way to
> read this device's counters):
>
> | toggle | frame total | delta |
> |---|---|---|
> | px=1, mirrors + mirror2 (his config) | 8.315 | — |
> | `--no-mirror2` | 8.247 | −0.07 |
> | `--no-mirror2 --no-mirror` | 7.018 | −1.23 (first-order mirrors) |
> | px=8 instead of px=1 | 5.244 | −3.07 (**tess density**) |
> | `--tess` off entirely | 4.755 | −3.56 |
>
> The G-buffer pass is where it lands: **0.57 ms (no tess) → 1.24 (px=8) → 4.26
> (px=1)**.
>
> **Residual, and labelled INFERRED:** his HUD read 9.68 ms where the offscreen
> repro reads 8.35 (p95 8.78). The ~0.9–1.3 ms gap is not accounted for. `GPU
> FRAME` is `[cb GPUEndTime] - [cb GPUStartTime]` of the *render* command buffer
> only (`Deferred.mm:3879`), so the blit/HUD command buffer is excluded from the
> number but still competes for the GPU alongside WindowServer compositing a
> 2560×1440 drawable; his figure is also a single frame, not a median. Neither
> explanation is measured.

![sweep](img/perf/greets_frame_time_spline_sweep.png)

## Method

```
cd Runtime
../build-gpu/GpuBench/GpuBench --fld=SCENES/GREETS.FLD --t=<T> --pass=deferred \
  --warmup=30 --iters=250 --tess --tess_px=8 --tess_presplit=8 \
  --spline --reanimate --no-draw
```

81 poses, `t = 0 … 8000` step 100 — the full part. 1920×1080 (matching
`Runtime/rev.cfg`), the user's feature set: hardware tessellation at px=8 P=8,
mirrors on, **second-order mirrors on**, `--reanimate` so the window's own
per-frame refresh path runs. Raw data: `data/greets_frame_time_spline_sweep.csv`.

**`--tess_px=8` here is the defect in this study, not a detail.** The user's own
window session ran `--tess_px=1`. See the correction box at the top; the px=1
arm of the same grid is in "The px=1 arm" below.

**`--spline` is load-bearing and was missed on the first two attempts.** Without
it the default `camPose` (the t=5743 review pose) PINS the camera and `--t` only
drives animation, so a "t-sweep" measures one camera with a moving mech. Those
runs looked flat at 5.0–6.7 ms for the wrong reason. Every number below is from
the spline arm.

## Result

| statistic | min | median | max |
|---|---|---|---|
| frame total, median of 250 | 3.46 | 5.37 | **6.15** |
| frame total, p95 of 250 | 3.84 | 5.53 | **6.28** |

Poses over 8 ms: **0**. Over 10 ms: **0**. Over 11 ms: **0**.

Worst poses by p95 (per-pass columns are each pass's own p95, ms):

| t | median | p95 | shadow | gbuffer | lighting | tonemap | mirror | mirror2 | tess-fac |
|---|---|---|---|---|---|---|---|---|---|
| 1700 | 6.15 | 6.28 | 0.54 | 1.78 | 2.60 | 4.00 | 2.54 | 2.66 | 0.028 |
| 3600 | 6.12 | 6.28 | 0.55 | 2.65 | 3.32 | 4.04 | 3.29 | 2.77 | 0.018 |
| 1400 | 6.06 | 6.23 | 0.55 | 1.81 | 2.62 | 3.92 | 2.54 | 2.65 | 0.028 |
| 5400 | 6.06 | 6.19 | 0.55 | 2.42 | 3.08 | 3.97 | 2.62 | 2.12 | 0.018 |
| 1100 | 5.96 | 6.16 | 0.56 | 2.11 | 2.85 | 3.73 | 2.81 | 2.91 | 0.032 |

**The per-pass columns do not sum to the frame total and must not be read that
way.** This device only supports `MTLCounterSamplingPointAtStageBoundary`, so
each number is an encoder's start-to-end span on the GPU timeline, and those
spans overlap heavily. The frame total is the only additive figure.

## The px=1 arm (2026-08-09)

Same spline, same 1920×1080, `t = 0 … 8000` step 500, **px=1 and px=8
interleaved within each rep** so the two arms see the same machine weather;
each cell is the min of 2 reps of the median of 60 frames.

| t | px=8 | px=1 | delta |
|---|---|---|---|
| 0 | 5.10 | 6.62 | +1.52 |
| 500 | 5.67 | 7.08 | +1.41 |
| 1000 | 5.54 | 6.13 | +0.59 |
| 1500 | 5.55 | 6.19 | +0.63 |
| 2000 | 5.59 | 6.18 | +0.59 |
| 2500 | 5.18 | **8.24** | +3.05 |
| 3000 | 4.79 | 5.63 | +0.84 |
| 3500 | 5.54 | 5.76 | +0.22 |
| 4000 | 5.55 | 5.73 | +0.18 |
| 4500 | 5.11 | 7.03 | +1.92 |
| 5000 | 5.12 | 6.61 | +1.49 |
| 5500 | 5.74 | 6.67 | +0.93 |
| 6000 | 3.66 | 6.54 | +2.88 |
| 6500 | 3.71 | 5.04 | +1.32 |
| 7000 | 5.70 | 5.81 | +0.11 |
| 7500 | 4.23 | 4.36 | +0.13 |
| 8000 | 4.15 | 4.36 | +0.21 |

The px=8 column reproduces the original study (max 5.74 here vs median 5.37 /
max 6.15 there, coarser grid), which is the cross-check that this run is not
itself junk. The px=1 column peaks at **8.24 ms at t=2500**; t=2841, measured
separately with 3 reps, is **8.35 ms**. The cost is strongly pose-dependent —
+0.11 ms at t=7000, +3.05 ms at t=2500 — because it tracks how much displaced
stone is on screen and how near it is.

## Measurement hygiene — why every number above says "min of N interleaved reps"

The px=1 sweep was first run as single passes per pose while two unrelated
`./DEMO` processes were pinning the machine (load average 15–29). It produced
p95 values up to **15.3 ms** and "10 of 33 poses over 11 ms" — pure
contamination: the same pose (t=500) read 7.22 ms minutes earlier and 7.08 ms in
the interleaved re-run. Those numbers are discarded and are not in this
document. **A single timed run on a loaded machine is not evidence here.**
Interleave the arms, take the min across reps, and sanity-check one arm against
a known-good previous measurement.

## Where the missing milliseconds are

Wall-clock differencing at the two worst poses — same process, same flags, only
`--iters` changed, so the difference is 1000 frames of the offscreen loop
including all CPU-side encode:

| pose | iters=200 | iters=1200 | per frame | GPU-only median |
|---|---|---|---|---|
| t=1700 | 3.184 s | 10.723 s | **7.54 ms** | 6.15 ms |
| t=3600 | 3.076 s | 10.358 s | **7.28 ms** | 6.12 ms |

So CPU-side encode + submit costs ~1.2–1.4 ms/frame on top of the GPU, and the
whole offscreen pipeline runs at **133–137 fps** at its worst pose.

- **MEASURED**: GPU passes ≤ 6.3 ms at every pose *at `--tess_px=8`*; offscreen
  wall ≤ 7.5 ms. **Superseded for px=1** — see the correction box: 8.35 ms.
- **MEASURED**: second-order mirrors cost +0.11 ms at px=8 and +0.07 ms at px=1
  (they are not the dip, at either knob).
- ~~**INFERRED**: 85–90 fps needs 11.1–11.8 ms. At least ~3.6–4.3 ms/frame is
  therefore spent in work the offscreen path does not do…~~ **RETRACTED.** The
  premise was that GPU pass cost tops out at 6.3 ms, and it does not: at the
  user's actual `--tess_px=1` it reaches 8.35 ms, so most of the gap the
  original text attributed to the window path is ordinary G-buffer work. The
  unexplained residual is now ~0.9–1.3 ms, not ~3.6–4.3 ms. (The vsync
  observation still stands on its own: a missed vsync on a 120 Hz display lands
  at 60 fps, not 85–90.)

## What to do next, in the window

The window HUD already prints per-pass GPU ms plus `CPU ANIM` / `UPLOAD`. Its
pass-name array used to have five entries while the loop ran over six passes —
an out-of-bounds read that printed a garbage label on the last row; that is
fixed, and the array is now `kPasses`-sized (seven, including `MIRROR2`).

When the dip happens, the HUD settles it in one glance:

- **`GPU FRAME` rises toward 11 ms** → it is GPU pass cost after all, at a pose
  this sweep did not sample (a free-fly pose off the authored spline).
- **`GPU FRAME` stays ~6 ms while FPS reads 85–90** → the GPU is idle and the
  cost is present/vsync or CPU-side; `CPU ANIM` and `UPLOAD` say which.

**And the first branch is the one that fired.** `GPU FRAME 9.680 MS` at t=2841
on the spline, with `--tess_px=1`. Before reaching for the window loop, check
the tess knob: `--tess_px=1` is roughly a **+3 ms** frame at the stone-heavy
poses versus the `--tess_px=8` default this document originally measured.
If the dip needs to go away, that is the dial — it is the G-buffer pass, not the
mirrors.
