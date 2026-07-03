# fountain — scene sources

The fountain scene (water fountain with spaceship). Ships as `Runtime/SCENES/FOUNTAIN.FLD`.

## Regenerate the FLD

From this directory:

```sh
# build the converter once (see tools/lwsread)
cmake -S ../../tools/lwsread -B ../../tools/lwsread/build && cmake --build ../../tools/lwsread/build

# convert using the legacy-VLUM build (shipping FLD was built before the b441da6 fix)
../../tools/lwsread/build/lwsread_legacy "FOUNTAIN - final.LWS" FOUNTAIN.FLD

# install
cp FOUNTAIN.FLD ../../Runtime/SCENES/FOUNTAIN.FLD
```

Produces a 512,654-byte, FldVersion-0.113 scene (24 objects, 14 lights).

Or via pin_scene.py (verifies byte-identity):

```sh
python3 ../../tools/pin_scene.py "FOUNTAIN - final.LWS" ../../Runtime/SCENES/FOUNTAIN.FLD \
  --legacy-vlum \
  --pick SHIP1.lwo=$(pwd)/SHIP1.lwo
```

## Files

| File | Role | Source in Original/ |
|------|------|---------------------|
| `FOUNTAIN - final.LWS` | scene (22 obj refs, 14 lights) | `Original/Scenes/CITY/S1FNT/FOUNTAIN - final.LWS` |
| `SHIP1.lwo` | high-poly spaceship 1 (423 KB, s1_side/s1_up textures) | `Original/dos-rev/REVIVAL/SCENES/CITY/SHIP1.LWO` |
| `Shp2.lwo` | spaceship 2 | `Original/Scenes/CITY/SHIPS/SHP2/SHP2.LWO` |
| `moutine.lwo` | mountain tile | `Original/Scenes/CITY/S1FNT/MOUTINE.LWO` |
| `fount.lwo` | fountain geometry | `Original/Scenes/CITY/S1FNT/FOUNT.LWO` |
| `pilon.lwo` | pilon/pillar | `Original/Scenes/CITY/S1FNT/PILON.LWO` |
| `inbal.lwo` | inbal object | `Original/Scenes/CITY/FOUNTAIN/INBAL.LWO` |
| `dio.lwo` | dio object | `Original/Scenes/CITY/FOUNTAIN/DIO.LWO` |

## Archaeology notes

The correct LWS is `FOUNTAIN - final.LWS` (14 lights, 22 obj refs = 24 unique instances),
not `FOUNTAIN.LWS` (8 lights, 20 obj refs) nor `FOUNT.LWS` / `FOUNTA~1.LWS` (8 lights).

The `SHIP1.lwo` must be the **dos-rev CITY** variant (423 KB, `spc1 front glass`
COLR=(156,162,175)). The dos-rev CHASING variant has COLR=(134,140,157) for that surface.

Most objects auto-resolve unambiguously from `Original/Scenes/CITY/S1FNT/`. Two objects
(`inbal.lwo`, `dio.lwo`) resolve from `Original/Scenes/CITY/FOUNTAIN/` (the S1FNT copies
have different content).

### Legacy VLUM converter

The shipping `FOUNTAIN.FLD` was built before commit `b441da6` ("greets: fix LWO luminosity
units"). Use `lwsread_legacy` (compiled with `-DLEGACY_VLUM=1`) or
`pin_scene.py --legacy-vlum` to reproduce the byte-identical result.
