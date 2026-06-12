# Per-scene parameter scripts

Every engine FeatureFlag (anything in `FDS/Base/FeatureFlags.def` — the
same names as the `--flags`) can be driven per scene, per time, from a
plain-text script that hot-reloads while the demo runs.

## Files

`Runtime/SCRIPTS/<scene>.params`, where `<scene>` is the name each scene
passes to `tickTabToggle` — currently: `city`, `chase`, `fountain`,
`crash`, `greets`, `conetest`, `mirrortest`. No file = no overrides.

## Syntax

```
# comment
fast_fog          = 1          # constant for the whole scene
fast_fog_density @ 0    = 2    # time key (scene Timer units)
fast_fog_density @ 600  = 8    # floats/ints LERP between keys
fast_fog_density @ 2800 = 1    # before first / after last key = clamped
```

- `name = value` — constant override, applied from scene start.
- `name @ time = value` — keyframe. Floats and ints interpolate linearly
  between keys; **bools step** (the lower key's value holds until the next
  key time). Mixing a constant line and keyed lines for the same param
  treats the constant as a key at t=0.
- Values: numbers, or `true/false/on/off` (CLI-lenient) for bools.
- Times are the scene `Timer` — the same units as `--snapshot=<scene>@t=N`
  and the on-screen `t=` indicator (city ≈ 0..2800).

## Semantics

- **CLI/env wins.** A flag explicitly set with `--flag…` or `FDS_*` env is
  never touched by a script (reported once at load) — A/B workflows and
  scripted scenes coexist.
- **Restore on exit.** Every value a script wrote is restored when the
  scene changes, the file is deleted, or the file is reloaded — scripts
  cannot leak into the next scene.
- **Hot reload.** The file's mtime is polled ~4×/s; save → `[SCRIPT]
  loaded …` on stderr and the new values apply immediately. Parse errors
  name the line and are skipped, the rest of the file still applies.
- **Kill switch:** `--no-param-scripts`.

## Limitations

- Flags read **once at init** (buffer allocations, scene preprocessing,
  e.g. `shadow_lightmap_res`) won't respond mid-scene; most per-frame
  render/fog/lighting params do. When in doubt, watch whether the effect
  follows a save.
- One value per (param, time); later duplicate lines add keys, they don't
  replace.
- Applied once per frame on the demo thread before rendering — render
  workers see a consistent value for the whole frame.

## Live tuning console

While the demo runs, `http://localhost:8666` (flags: `tune_server`,
`tune_port`) serves a knob page over the same registry: every flag
grouped by category with sliders/checkboxes, search, and tooltips.
Edits apply immediately and count as *explicitly set* — scripts yield
to them exactly like CLI flags — until the row's ↺ release button
hands control back. Badges: green dot = the scene script drives this
param right now (knobs animate at ~2.5 Hz as keyed ramps play);
orange diamond = time-keyed in the script (bake won't touch it).

**bake to <scene>.params** writes every console-tuned knob straight
into the scene script on disk — replacing the param's constant line
or appending one — and releases the knobs to it in the same motion
(same values, no flash; the script hot-reloads within a few frames).
No copy/paste: tune by eye, hit bake, the look is committed to the
file. "copy CLI" remains for A/B command lines.
