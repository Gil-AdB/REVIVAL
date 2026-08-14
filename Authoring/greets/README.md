# greets — scene sources

The greets scene ("Jenia in pyramid": pyramid + walking mech + 10 lights, with
three text-display screens). Ships as `Runtime/SCENES/GREETS.FLD`.

## Regenerate the FLD

From this directory:

```sh
# build the converter once (see tools/lwsread)
cmake -S ../../tools/lwsread -B ../../tools/lwsread/build && cmake --build ../../tools/lwsread/build

# convert (object .lwo files must be in the CWD — they are, here)
../../tools/lwsread/build/lwsread JENINPYR-new-2.LWS GREETS.FLD

# install
cp GREETS.FLD ../../Runtime/SCENES/GREETS.FLD
```

Produces a 233,315-byte, FldVersion-0.113 scene (9 objects, 10 lights).

## Light ranges are authored HERE

Each `AddLight` block in `JENINPYR-new-2.LWS` carries `LightRange <f>`. That is
the **single source of truth** for the light's radius — it flows

```
LWS "LightRange"  →  tools/lwsread  →  FLD Range envelope  →  Omni::Range
                  →  Omni::IRange (evaluated per frame by Animate_Objects)
```

and nothing in the engine or in `DEMO/GREETS.CPP` rewrites it. To retune a
light, edit its `LightRange` here and regenerate — that is the whole procedure.

All ten currently author **30**. Until 2026-08-06 they did not: the LWS carried
the 1998 values `3 / 3 / 10 / 10 / 7 / 20 / 20 / 2 / 2 / 2` and two code-side
patches sat on top of them, the second undoing the first —

1. `SceneCorrections()` scaled each Range spline by
   `{2, 2, 2, 2, 2, 1.7, 1.7, 2, 2, 2}` ("a crazy hack used to adjust
   omnilights in code"), giving `6 / 6 / 20 / 20 / 14 / 34 / 34 / 4 / 4 / 4`;
2. `Initialize_Greets` then tested `IRange == 0` and overwrote **both**
   `IRange` and `Range.Keys[0]` with a flat `30`. That test was a bug: at scene
   init `IRange` is 0 because `Animate_Objects` has not run yet, not because
   the content lacks a range (see `FDS/FLD/FLD_CONV.CPP`). So it fired on all
   ten every run and step 1 and the authored numbers were both discarded.

The rendered look was therefore a flat 30, and `30` is what the LWS now says.
The change is a pure refactor — flat-30 LWS with both patches removed renders
**byte-identical** to the pin `f1297141611c484bac7cc10a8bdcf630` (3/3 runs).
The 1998 numbers remain in the read-only reference copy
`Original/Scenes/CITY/INPYR/JENINPYR-new-2.LWS`.

`FDS_GREETS_OMNI_DEFAULT_RANGE` / `--greets_omni_default_range` survives as a
tuning dial only: default 0 = inert, `> 0` force-overrides every omni's range.

## Files

| File | Role |
|------|------|
| `JENINPYR-new-2.LWS` | the scene (camera/light/object tracks). LWSC v1. The `-new-2` variant is the live one — it reproduces the high-poly `out1.fld` byte-for-byte with the high-poly pyramid. |
| `Piramid.lwo` | **low-poly** pyramid (3704v/5532f) **with the screen-orientation fix applied** (see below). |
| `Hull.lwo`, `Hull2.lwo`, `L_leg1.lwo`, `L_leg2.lwo`, `R_leg1.lwo`, `R_leg2.lwo` | the walking mech. |

The `.lws` references objects via DOS paths (`c:\lwave\city\INPYR\...`); the
converter strips them to basename, so the files just need to sit here named as
above.

The `.lwo` files used to store full DOS `TIMG`/`RIMG`/`TALP` paths
(`C:\LWAVE\CITY\INPYR\...`); these have been rewritten to plain basenames
(e.g. `MECH_HUL.JPG`). The 12 referenced textures are also copied here so
LightWave can open the scene without needing the original DOS directory tree.
The engine resolves textures at runtime from `Runtime/TEXTURES/` (uppercasing
the name on load), so the copies in this directory are authoring-only — they
are tracked in git but not installed by the build.

## The screen-orientation fix (low-poly only)

The original low-poly pyramid had two text screens mirrored: their LWOB surface
texture-projection **size** had the wrong sign on one axis. The high-poly rebuild
had corrected them; this `Piramid.lwo` carries the same correction:

| Surface | wrong | fixed (matches high-poly) |
|---------|-------|---------------------------|
| `screen 3` | TSIZ.Z = +14.5508 | **−14.5508** |
| `screen 4` | TSIZ.X = +2.5921  | **−2.5921**  |

(A negative LightWave texture size mirrors the projected image along that axis.)
This was applied as a sign-bit flip on the two big-endian floats in the
`screen 3` / `screen 4` SURF chunks — geometry untouched. The pristine,
unfixed low-poly source is `Original/Scenes/CITY/INPYR/PIRAMID-orig.LWO`
(read-only reference). The high-poly improved mesh is
`Original/Scenes/CITY/INPYR/PIRAMID.lwo` (1.28 MB) — kept for reference but not
needed now that deferred per-pixel lighting removes the need for high-poly-for-Gouraud.

## Editor write-back

The browser surface editor (`DEMO.html?editor`, served by
`tools/editor_server.py`) persists material edits **into these .lwo files**:
"Save to LWO" POSTs the changed surface values; the server patches the SURF
chunks (`tools/lwopatch.py` — updates or inserts DIFF/VDIF, SPEC/VSPC, GLOS,
LUMI/VLUM, TRAN/VTRN, REFL/VRFL, COLR), copies the previous file into
`.backups/<name>.<timestamp>.lwo` (gitignored), reruns `lwsread`, and installs
the new `GREETS.FLD` into `Runtime/SCENES/`. "Reload scene" re-boots the page;
the editor fetches the live FLD at boot, so saved values show immediately —
native runs pick them up at next launch.

Specular/glossiness for `rooms` (marble wall: 0.1/64) and the shiny set
(`stairs`/`amudim`/`floor`/`hull`/`hull not smooth`: 0.4/48) are authored here
in the LWOs — they used to be code-side bumps in `DEMO/GREETS.CPP`, baked into
the sources when write-back shipped so saved edits can't be stomped at load.
