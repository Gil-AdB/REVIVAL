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

Produces a 233,272-byte, FldVersion-0.113 scene (9 objects, 10 lights).

## Files

| File | Role |
|------|------|
| `JENINPYR-new-2.LWS` | the scene (camera/light/object tracks). LWSC v1. The `-new-2` variant is the live one — it reproduces the high-poly `out1.fld` byte-for-byte with the high-poly pyramid. |
| `Piramid.lwo` | **low-poly** pyramid (3704v/5532f) **with the screen-orientation fix applied** (see below). |
| `Hull.lwo`, `Hull2.lwo`, `L_leg1.lwo`, `L_leg2.lwo`, `R_leg1.lwo`, `R_leg2.lwo` | the walking mech. |

The `.lws` references objects via DOS paths (`c:\lwave\city\INPYR\...`); the
converter strips them to basename, so the files just need to sit here named as
above. Textures (`P_TEXT.JPG`, `MARB*.JPG`, `P*.JPG`, …) are not needed for
conversion — they're recorded by name and resolved at runtime from
`Runtime/TEXTURES/`.

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
