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
| chase | `CHASE.FLD` (747 KB) | candidate: `…/CHASING/CHASE.LWS` | ⏳ not yet validated. |
| fountain | `FOUNTAIN.FLD` (512 KB) | candidates: `…/S1FNT/FOUNTAIN.LWS`, `…/FOUNTAIN/FOUNT.LWS` | ⏳ not yet validated. |
| crash | `CRASH.FLD` (2 KB) | — | ⏳ tiny; likely largely code-built, no obvious `.lws`. |

To pin a pending scene we use the same method that worked for greets: convert a
candidate `.lws` with the matching objects and `cmp` against the shipping FLD
(0.113), iterating on object versions/variants until byte-identical.
