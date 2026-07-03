# Authoring — scene sources for the shipping FLDs

One subdirectory per demo scene, each holding the **exact** LightWave sources
(`.lws` scene + `.lwo` objects) that convert to that scene's
`Runtime/SCENES/*.FLD`, plus a `README.md` with the regenerate recipe. The goal:
next time we want to change a scene, edit here, reconvert, drop the FLD in.

## Pipeline

```
LightWave .lws + .lwo  ──►  tools/lwsread  ──►  *.FLD  ──►  Runtime/SCENES/  ──►  demo
```

`tools/lwsread` is the native LWS→FLD converter (a 64-bit port of the original
1998 `LWSREAD.EXE`; see `tools/lwsread/CMakeLists.txt`). Build once:

```sh
cmake -S tools/lwsread -B tools/lwsread/build && cmake --build tools/lwsread/build
```

Then, per scene, `cd Authoring/<scene>` and run the command in its README.
Objects must sit in the CWD named as the `.lws` references them (DOS paths are
stripped to basename). The shipping FLDs are FldVersion **0.113** — same as the
converter — so a correct source set reproduces them byte-for-byte.

## Scene status

| Scene | Ships as | Source set | Status |
|-------|----------|-----------|--------|
| **greets** | `GREETS.FLD` (232 KB, low-poly) | `greets/JENINPYR-new-2.LWS` + pyramid + mech | ✅ identified & validated (converter reproduces `out1.fld` byte-for-byte; low-poly screen-orientation fix applied — see `greets/README.md`). Pending live visual confirmation. |
| city | `CITY.FLD` (977 KB) | candidates: `Original/Scenes/CITY/CITY.LWS`, `CITY_J~1.LWS` | ⏳ exact source not yet pinned (the hard one). |
| **chase** | `CHASE.FLD` (747 KB, 80 objects, 7 lights) | `chase/CHASE.LWS` (dos-rev, 18 camera keys) + dos-rev mountain/ship variants | ✅ byte-identical with `lwsread_legacy --legacy-vlum` (see `chase/README.md`). |
| **fountain** | `FOUNTAIN.FLD` (512 KB, 24 objects, 14 lights) | `fountain/FOUNTAIN - final.LWS` (S1FNT, 22 obj refs) + dos-rev CITY SHIP1 | ✅ byte-identical with `lwsread_legacy --legacy-vlum` (see `fountain/README.md`). |
| **fountain-extended** | historical `FOUNTAIN.FLD` (141 KB, git `0bf67d4`; shipped 2018-2020) | `fountain-extended/FOUNTA~1.LWS` + FOUNTAIN-dir objects + low-poly CITY SHIP1 | ✅ byte-identical with `lwsread_ofir` / `pin_scene.py --ofir` (OFIR-era `LastFrame` keyword; see `fountain-extended/README.md`). |
| crash | `CRASH.FLD` (2 KB) | — | ⏳ tiny; likely largely code-built, no obvious `.lws`. |

To pin a pending scene we use the same method that worked for greets: convert a
candidate `.lws` with the matching objects and `cmp` against the shipping FLD
(0.113), iterating on object versions/variants until byte-identical.
`tools/pin_scene.py <candidate.LWS> <shipping.FLD>` automates one attempt
(stages basename-matched objects from `Original/`, converts, reports the size
delta + first differing byte + ambiguous object variants to `--pick` from).

## Editor write-back without pinned sources

First pin attempts on chase came out ~half the shipping size (wrong-generation
object variants), so the editor doesn't wait for the archaeology: for city /
chase / fountain / crash, `tools/editor_server.py` patches the **shipping FLD
binary directly** via `tools/fldpatch.py` (a byte-exact walker of the v0.113
layout from `tools/lwsread/FLDSAVE.CPP` — it refuses to touch a file whose
walk doesn't land exactly on EOF). Surface props and point-light
color/intensity/range both persist; backups + restore live under
`Runtime/SCENES/.backups/`. If a scene's sources are pinned later, flip it to
`authoring: True` in the server registry and it upgrades to source-level
write-back like greets.
