# LWO Surface Editor — Implementation Plan

A browser-based tool to edit a scene's LightWave `.lwo` **surfaces** live, pick a
surface by clicking the rendered frame, and save the edit back to the authoring
`.lwo` — then regenerate the shipping scene through the existing converter. The
editor runs **in the browser** via the engine's Emscripten/WASM build ("online").

This document is written to be handed to an agent who has not seen the prior
discussion. Read §1 and §2 first; they're the load-bearing facts.

---

## 0. Goal & non-goals

**Goal:** select a surface (by clicking the rendered scene or from a list), tune
its surface properties (color, diffuse, specular, glossiness, luminosity,
reflection, transparency, texture maps, smoothing) with **instant visual
feedback**, and **persist** the change so it survives a rebuild.

**In scope (minimal version):** editing **surfaces only** — the `SURF` chunks of
the `.lwo`. No geometry editing.

**Out of scope (deferred):** editing geometry (`PNTS`/`POLS` — vertex positions,
faces, smoothing topology). A full geometry writer is a separate, larger effort.
⚠️ Note this means the editor as scoped **cannot** fix mesh faceting like the
greets mummy (that's geometry/normals); see §8.

---

## 1. Background: the pipeline and what already exists

**The runtime never loads `.lwo`/`.lws` — it loads `.FLD`.** FLDs are generated
offline:

```
LightWave .lws + .lwo  ──►  tools/lwsread  ──►  *.FLD  ──►  Runtime/SCENES/  ──►  demo
```

- **`tools/lwsread/`** — native LWS→FLD converter, a 64-bit port of the 1998
  `LWSREAD.EXE`. Key files:
  - `LWOREAD.CPP` — parses the LWO IFF chunk tree (this is the reader we invert).
  - `LWSREAD.CPP` — parses the `.lws` scene (objects, lights, motions).
  - `FLDSAVE.CPP` — writes the `.FLD`.
  - `LWREAD.H` — the `FldMat` struct + chunk constants.
  - Build: `cmake -S tools/lwsread -B tools/lwsread/build && cmake --build tools/lwsread/build`.
  - FLD output is **FldVersion 0.113**; a correct source set reproduces the
    shipping FLD **byte-for-byte** (this is our validation lever).

- **`Authoring/<scene>/`** holds the `.lws` + `.lwo` sources + a `README.md` with
  the per-scene regenerate recipe. greets is fully identified
  (`JENINPYR-new-2.LWS` + `Piramid.lwo` + mech `.lwo`s); city/chase/fountain are
  not yet pinned (irrelevant for v1, which targets greets).

- **WASM build already exists but is stale.** `DEMO/CMakeLists.txt` has an
  `if(EMSCRIPTEN)` branch (`-sUSE_SDL=2`, `-msimd128`, pthread stack bump);
  `DEMO/MainLoop.cpp` drives the frame via `emscripten_set_main_loop`;
  `DEMO/REV.CPP` has `#ifdef __EMSCRIPTEN__` asset/init paths. Artifacts
  (`WASMEXE.ZIP`, `FDS/wasm.exe`) predate threading + deferred kernel + mirrors +
  HDR, so the build needs reviving (Phase 0, the main risk).

- **Live material editing already works.** The deferred kernel reads `Mat->X`
  every frame, so mutating an in-memory `Material` shows up **instantly** — no
  re-bake of the scene needed. Materials live in the global `MatLib`
  (`FDS/Base/FDS_VARS.H:243`, populated/refreshed by `Scene_RebuildMatTable`).

- **Picking is nearly free.** The deferred G-buffer stores `matId` per pixel
  (see `DeferredSurfaceKernel.cpp`, `gb.matID` / the `viz_matid` path). A click →
  read `matId` at that pixel → resolve to the surface. We only need a
  `matId → surface-name` map (and to be aware of matID aliasing after
  `Scene_RebuildMatTable` tail-append — see the project memory on mat-table
  registration).

---

## 2. The LWO surface format (what the writer edits)

LWO is **IFF**: a `FORM` container holding length-prefixed chunks. Top level:
`PNTS` (vertices), `POLS` (faces), `SRFS` (the surface-name list), then one
**`SURF`** block per surface, each holding value sub-chunks. The full set the
reader understands (from `LWOREAD.CPP` `ChunkData[]`) — i.e. the editable
surface fields:

| Sub-chunk | Meaning | Engine `Material` field |
|-----------|---------|--------------------------|
| `COLR` | base color (RGB + pad) | `BaseCol` |
| `FLAG` | surface flags (bit 4 = `Surf_Smoothing`, `Surf_DoubleSided`…) | `TFlags` → `Flags` (`Mat_RGBInterp`, `Mat_TwoSided`) |
| `LUMI`/`VLUM` | luminosity (int %/float) | `Luminosity` |
| `DIFF`/`VDIF` | diffuse | `Diffuse` |
| `SPEC`/`VSPC` | specular | `Specular` |
| `REFL`/`VRFL` | reflection | (reflection amount) |
| `TRAN`/`VTRN` | transparency | `Transparency` |
| `GLOS` | glossiness | `Glossiness` |
| `RFLT` | reflection mode | `ReflectionMode` |
| `RIMG`/`RSAN`/`RIND`/`EDGE` | reflection image / seam angle / refractive index / edge transp. | — |
| `SMAN` | **max smoothing angle (RADIANS)** | `MaxSmoothingAngle` |
| `CTEX`/`DTEX`/`STEX`/`RTEX`/`TTEX`/`BTEX` | color/diffuse/spec/refl/transp/bump texture map blocks | texture paths |
| `TIMG`/`TFLG`/`TSIZ`/`TCTR`/`TFAL`/`TVEL`/`TFRQ`/`TALP`/`TWRP`/`TAAS`/`TOPC`/`TFP0`/`TFP1` | texture image/flags/size/center/etc. | texture params |

Notes:
- Int chunks store 0..255 (= percent×255); the `V*` float variants store IEEE
  float. Prefer rewriting whichever variant the source already used.
- `FLAG` (smoothing bit) + `SMAN` (angle) are the LightWave smoothing controls —
  the same data behind the mummy faceting discussion. Editing them here is
  surface-level; it still won't add geometry.
- IFF chunk lengths are big-endian and **even-padded**; any value-size change
  requires recomputing the enclosing `FORM`/`SURF` lengths.

---

## 3. Architecture

```
┌─────────────────────────────  Browser  ──────────────────────────────┐
│  <canvas>  ← WASM renders the scene (free-cam, no audio)              │
│     │ click(x,y)                                                      │
│     ▼                                                                 │
│  Embind JS API:                                                       │
│     getSurfaces()            → [{name, props…}]                        │
│     setSurfaceProp(name,k,v) → mutate MatLib Material + re-render      │
│     pickSurface(x,y)         → surface name (via gbuffer matId)        │
│     exportLWO(objName)       → patched .lwo bytes                      │
│     │ Save                                                            │
│     ▼                                                                 │
└─────┼─────────────────────────────────────────────────────────────────┘
      ▼
  Authoring/<scene>/*.lwo   ──►  tools/lwsread  ──►  *.FLD  ──►  Runtime/SCENES/
      (optional auto-reconvert → reload)
```

**Components to build:**

- **A. WASM editor build target** — revive the Emscripten build; render a chosen
  scene to the canvas, free-cam, audio off. Likely a thin entry (reuse
  `MainLoop.cpp`) plus a `--editor`/build-define gate.
- **B. LWO writer (C++)** — inverse of `LWOREAD.CPP`. Minimal version **patches
  only `SURF` sub-chunk values** in the original `.lwo` byte buffer, leaving
  `PNTS`/`POLS`/`SRFS` and chunk order untouched; recompute IFF lengths. A full
  geometry writer is explicitly deferred.
- **C. Embind JS API** — `getSurfaces` / `setSurfaceProp` (mutate the live
  `Material` in `MatLib`; re-bake the normal map if a texture path changes;
  trigger a re-render) / `pickSurface` / `exportLWO`.
- **D. Picking** — map the per-pixel G-buffer `matId` to a surface name.
- **E. Front-end** — canvas + property panel (color picker, sliders, text fields,
  texture path) + Save. Keep it minimal; this is a dev tool.

---

## 4. Phases & effort

- **Phase 0 — Revive the WASM build (GATING).** Get the *current* engine
  compiling and running in-browser: render one scene (greets) to canvas with a
  free-cam, audio disabled. Validate it renders. *This is the main unknown — the
  engine grew threading, the deferred kernel, mirrors, and HDR since the last
  WASM artifacts.* Decide the threading story under emscripten pthreads up front
  (single-thread fallback is acceptable for an editor).
- **Phase 1 — Surface enumeration + live edit.** Embind `getSurfaces` /
  `setSurfaceProp`; mutate `MatLib`; confirm instant re-render. No persistence
  yet. Cheap once Phase 0 lands.
- **Phase 2 — Click-to-pick** surface from the canvas (gbuffer `matId` →
  surface name). *(~0.5–1 day)*
- **Phase 3 — LWO writer + save loop.** `exportLWO` patches `SURF` chunks; full
  round-trip (edit → save → reconvert via `lwsread` → FLD identical except the
  intended deltas). *(~1–2 days)*
- **Phase 4 — Polish:** color picker, texture swap + normal re-bake,
  object/scene selector, mouse-look camera, undo/redo, optional auto-reconvert →
  reload. *(ongoing)*

---

## 5. Validation (do these, in order)

1. **Identity round-trip (writer correctness):** load `.lwo` → make *no* edits →
   `exportLWO` → `cmp` against the source file. Must be byte-identical (proves
   the writer is a faithful inverse before any delta is trusted). If perfect
   byte-identity is impractical (chunk reordering), fall back to: reconvert both
   original and re-saved `.lwo` through `lwsread` and `cmp` the two FLDs.
2. **Single-field delta:** change one property → `exportLWO` → reconvert →
   diff the FLD against the baseline; only the intended surface field changes.
3. **Native parity:** the regenerated FLD renders identically in the native demo
   (the WASM renderer is the editor, the native demo is what ships).

---

## 6. Risks (ranked)

1. **WASM build health** — the Phase 0 gate. Threading under emscripten pthreads
   is the likely sticking point; be ready to run the editor single-threaded.
2. **IFF surgery correctness** — recomputing `FORM`/`SURF` lengths, even-padding,
   big-endian; inserting a sub-chunk that didn't exist (shifts offsets). The
   identity round-trip test (5.1) catches regressions here.
3. **`matId → surface` stability** — `Scene_RebuildMatTable` tail-appends and can
   alias matIDs (see the mat-table memory). Build the map *after* the table is
   final and re-derive it on reload.
4. **Texture edits** — changing a texture path must re-load + (for normal/bump)
   re-bake, and respect the block-tiled ("shachletz") layout the rasterizer
   expects (see `Scene_MakeTiledTexture` / the texture-tiling memory).

---

## 7. SURF (LWO writer) vs `.params` material-override — persistence decision

We discussed **two** ways to persist edits. They are not mutually exclusive — the
editor UI can target either backend — but the **persistence target should be
chosen before Phase 3.** Noting the diff here; decide later.

| Aspect | **SURF / LWO writer** (this plan) | **`.params` material-override** |
|---|---|---|
| What it edits | The authoring `.lwo` source | A runtime override file (sibling of `SCRIPTS/<scene>.params`), applied at scene init |
| Round-trips to LightWave | **Yes** (reopen the `.lwo`) | No |
| Needs a binary writer | **Yes** (invert `LWOREAD`, IFF chunk surgery) | No (text file) |
| Reuses existing infra | New writer | **`TuneServer`** already serves a browser knob page + "bake to disk" into `.params`; extend it to materials |
| Source of truth | The `.lwo` → regen FLD via `lwsread` | Layered on top of the FLD at load time |
| Effort | Higher (writer + round-trip validation) | Lower |
| Hot-reload | Via reconvert → reload | Direct (`.params` hot-reload exists) |
| Geometry edits later | Possible (full geometry writer) | Never (materials only) |
| Clobber risk | None (source is canonical) | None, but the override is *separate* from the FLD; a reconvert won't include it |

**Framing for the decision:**
- Choose **SURF/LWO writer** if the goal is a true authoring round-trip (edits
  live in the canonical source, reopenable in LightWave) and you eventually want
  geometry editing.
- Choose **`.params` override** if the goal is fast look-tuning that ships with
  the scene without LightWave and without a binary writer — it's much cheaper and
  reuses `TuneServer`'s existing browser UI + bake-to-disk.

A reasonable middle path: build the editor UI + picking + live-edit (Phases 0–2)
**backend-agnostic**, then implement whichever persistence backend we pick in
Phase 3.

---

## 8. Relationship to the greets mummy faceting (context)

The mummy ("momy" surface) faceting that prompted recent work is a **geometry**
issue (vertex normals / smoothing topology / low-poly silhouette), not a surface
property. The minimal editor (surfaces only) edits `FLAG`/`SMAN` (smoothing
on/off + angle) but cannot change tessellation. Fixing the mummy's geometry needs
either the deferred **full geometry writer** or a code-side mesh op — track that
separately from this editor.
