# The headless interactive repro harness (`--repro`)

**Read this before concluding "the defect doesn't reproduce headlessly".** It exists
because that conclusion was reached twice and was wrong at least once.

Every other diagnostic in this tree renders through `--snapshot`, which pins the
scene clock and camera and renders **one tick from a cold scene**. That is a real
blind spot, and this harness closes it — but the first thing this document has to
say is that the blind spot was *not* the cause of the defect it was built for. Use
the harness to **measure** whether a defect is interactive-only. Do not assume it.

Companion reading: `GRAPHICS_PIPELINE.md` (§8 headless validation), `ENGINE.md`.

---

## 1. What `--snapshot` actually pins

From `REV.CPP` (the `ParseSnapshotArgs` branch):

| Pin | Effect |
|---|---|
| `g_fineSceneClock = false` | the sub-tick float clock `g_FrameTimeF` is nailed to the integer `Timer`. Interactively it runs an EMA rate estimator and is generally *not* equal to `t`. |
| `g_occlSnapshotInert = true` | the prev-frame chunk-occlusion cull is disabled outright. |
| one tick, cold | nothing that accumulates over frames has converged. |

Things that accumulate and that a single cold tick therefore cannot express:
`Face::LastMip` under `--mip_hysteresis`, the mirror RTT, temporal froxel
reprojection (`--fast_fog_froxel_temporal`), async dynamic shadow bakes joined a
frame late, one-shot probe bakes whose result depends on where the camera was when
they first ran, and the greets text generator (`OldBuf` — an explicitly recursive
smear pipeline).

`--repro` runs the **real** scene driver and the **real** `tick()` for as many
frames as it takes, applies none of those pins, and scripts the transport keys into
the same global `Keyboard[]` array the SDL event pump writes. It does not poke
`Timer` behind the driver's back — that is exactly the mistake that made the old
`TimerProc` scrub a silent no-op (see the block comment on `TimerProc` in `REV.CPP`).

---

## 2. Interface

```sh
cd Runtime
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
FDS_GREETS_CAM="-14.1865349,2.84484363,-53.351326,0.998402655,-0.0386195704,0.0412385091" \
./DEMO --repro=greets@t=2993 --out=/tmp/rp --repro_from=0 \
       --repro_xres=1512 --repro_yres=848 --profiler=0 \
       <the user's scene flags...>
```

Output: `<out>/repro_greets_t<NNNNNN>.ppm`, named by the **actual** scene time
reached, plus `_sNN.ppm` for the `--repro_seq` series.

| Flag | Default | What it is for |
|---|---|---|
| `--repro_from` | 0 | scene time the session starts at. **The history-depth dial** — see §4. |
| `--repro_settle` | 8 | frames parked at the pose, no keys down, before the dump. A user reporting a defect is parked, not mid-scrub. |
| `--repro_seq` | 0 | dump N more consecutive frames at the same pose — the test for a defect that only appears on *some* frames. |
| `--repro_play` | 0 | reach the target with the clock free-running (`TimerInit(100)`) instead of scrubbing. Most faithful, deliberately nondeterministic, costs real time. |
| `--repro_xres` / `--repro_yres` | 0 | match the user's **window**, without editing `rev.cfg`. |
| `--repro_max_frames` | 20000 | safety budget; an unreachable target fails loudly. |

`--repro` is in `REV.CPP`'s headless-mode prefix list, so it forces
`SDL_VIDEODRIVER/AUDIODRIVER=dummy` and disables music. It can never open a window.

### It lands on `t` exactly

A paused F2 scrub steps a fixed **+10** ticks, so reachable times form a lattice —
and `GreetsScene::init()` does a bare `Timer++` before seeding `TTrd`, offsetting
it. A user's F9 pose is an arbitrary `t`. The harness therefore chooses its start
time so the lattice lands exactly on the first target, and prints

```
[REPRO] requested t=2993 -> LANDED t=2993 EXACT (308 frames, ...)
```

If it ever says `*** OFF-TARGET ***`, the frame is not the user's frame — `t+8` has
animated. Files are named by the time actually reached, never the requested one.

---

## 3. What it found for the t=2993 diagonal — and the correction

The brief was: long diagonal shading discontinuities across the greets stone wall,
visible in the user's live run, **twice** failing to reproduce under `--snapshot`.

**The harness reproduces the user's frame.** At his exact pose, flags and 1512x848
window, the wall matches his dump to **0.35–0.46 % of pixels differing by >8**
(mean 0.5/255) — the residual sitting on the animated mech and text panel.

**But `--snapshot` reproduces it too.** Measured at 1920x1080, snapshot vs the full
interactive path, on the wall crop:

```
diff>8: 0.0000 %      max diff: 2/255
```

and the diagonal is plainly present in the band-passed crop of **both**. So the
premise — that this defect is invisible to `--snapshot` — is false. I did not
reproduce the earlier negative and I do not know what differed in that recipe.
Two traps that produce exactly that false negative are documented in §5; at least
one of them (zsh word-splitting) silently ran flagless renders in my own first
attempt at the A/B battery.

The harness was not wasted: it *did* show real interactive-only divergence —
whole-frame, snapshot vs interactive differ on **42.98 %** of pixels (1.33 % by >8)
— but all of it is animated content (the recursive text generator, the mech, the
profiler HUD), none of it the wall. That is the useful negative: **for this defect,
accumulated per-frame state is not the mechanism.**

### History depth is measurable

`--repro_from` bisects it. Same pose, same target, different amounts of history:

| run | frames | vs the other |
|---|---|---|
| `--repro_from=2900` | 19 | — |
| `--repro_from=0` | 308 | 0.98 % of pixels differ by >8 |

So history does change the frame — just not the wall.

---

## 4. Root cause of the diagonal: `--pom_shell`'s per-vertex `ShellH`

Stage-off A/B at the pose (all `--snapshot`, since it reproduces):

| arm | diagonal? |
|---|---|
| full user flags | **yes** |
| `--no-pom_normal` (normal map off) | yes |
| `--no-greets_displace` | yes — *byte-identical to baseline*, displacement is not even on |
| no prism (`--pom_prism` dropped) | yes |
| `--pom_shell_lid_edge=0` | yes |
| plain parallax march, **no `--pom_shell`** | **no** |
| minimal `--pom_shell` + plain march | **yes** |

`--pom_shell` is necessary and sufficient. The `--wire_viz=1` overlay then settles
the geometry: the block is **one quad, two triangles**, and the shading crease lies
**exactly on the triangulation diagonal**.

Mechanism:

- `DEMO/MeshOps.cpp:4977` stamps a **per-vertex** scalar
  `V[i].ShellH = 0.5f + 0.5f * ndv`, where `ndv = mean(N_vertex · N_face)`. It is
  the height that vertex actually reached relative to its incident faces' planes,
  and carrying it as an interpolant is (per the comment there) "the whole point".
- The rasterizer transports it **per triangle**, perspective-correct, like UZ/VZ:
  `FDS/FILLERS/Mekalele.h` — `shZ1 = v1.ShellH * v1.RZ`, `da[11]`, `db[11]`. The
  shell march's **entry height** is this per-pixel interpolated value.

A per-vertex scalar linearly interpolated over a quad's two triangles is C0 but has
a **gradient discontinuity along the shared diagonal** unless the four corner values
happen to be bilinearly consistent (`h1 + h3 == h2 + h4`). On the greets wall the
four corners of a block have different smoothed normals, so they get different
`ndv`, so the entry height **kinks along the diagonal** — the march starts deeper on
one triangle than the other, its hit kinks, and the shading shows the crease.

That also explains the *earlier* unreproduced report, which was described as a
"quad-diagonal **normal** discontinuity" — same edge, same cause.

**Where the fix belongs: the POM shell.** Not the mip path, not the checkerboard,
not a temporal bake. Candidate directions (none implemented — `DEMO/MeshOps.cpp` and
`FDS/FILLERS/` are another owner's files):

1. Make the entry height **per-face** instead of a per-vertex interpolant (drops the
   corner correction the interpolant exists for).
2. Force the quad's four `ShellH` to be bilinearly consistent at build time, or
   split shelled quads symmetrically (centre vertex, 4 triangles) so no single
   diagonal carries the whole kink.
3. Reconstruct the entry height per pixel from geometry rather than interpolating a
   per-vertex scalar.

---

## 5. Traps that manufacture a false negative

1. **zsh does not word-split unquoted variables.** `FLAGS="--deferred --pom_shell"`
   then `./DEMO $FLAGS` passes the whole string as **one** argument; the binary
   prints `[FLAGS] unknown flag '...' (ignored)` and renders with **no flags at
   all**. Three "stage-off" renders came back byte-identical because of this. Use
   arrays: `FLAGS=(--deferred --pom_shell)` … `"${FLAGS[@]}"`, and **grep the log
   for `unknown flag`** before trusting any A/B.
2. **HDR silently eats the viz flags.** greets defaults to HDR, and the tonemap
   overwrites VPage at the end of the frame, so `--viz_normal` / `--viz_geonormal`
   render as plain albedo. Add `--no-hdr` (`GRAPHICS_PIPELINE.md` §4).
3. **Resolution is not cosmetic.** The user's window (1512x848) is not `rev.cfg`
   (1920x1080), and it moves mip selection, the checkerboard lattice and POM step
   counts. Use `--repro_xres/--repro_yres`; **do not edit `rev.cfg`** — it is shared
   state and editing it has already cost a diagnosis.
4. **The profiler HUD pollutes diffs.** The user's dumps have no HUD; a snapshot
   does. Pass `--profiler=0` or the top-left corner dominates the comparison.
5. **A whole-crop Radon locks onto the mortar courses**, which are present in every
   render and discriminate nothing. Restrict the angle sweep to the diagonal band,
   or just band-pass one block face and look.

---

## 6. Adding a scene

`RunRepro` is scene-generic apart from the `Initialize_*` / `create*Scene` pair and
the camera env var; both are in one `if` at the top of `RunRepro`
(`DEMO/ReproHarness.cpp`). The driver contract it relies on is `SceneDriver`'s —
`init()` seeds `TTrd = Timer`, `tick()` returns false at `partTime`.
