# chase — scene sources

The chase scene (spaceship flight over city/mountains). Ships as `Runtime/SCENES/CHASE.FLD`.

## Regenerate the FLD

From this directory:

```sh
# build the converter once (see tools/lwsread)
cmake -S ../../tools/lwsread -B ../../tools/lwsread/build && cmake --build ../../tools/lwsread/build

# convert using the legacy-VLUM build (shipping FLD was built before the b441da6 fix)
../../tools/lwsread/build/lwsread_legacy CHASE.LWS CHASE.FLD

# install
cp CHASE.FLD ../../Runtime/SCENES/CHASE.FLD
```

Produces a 747,511-byte, FldVersion-0.113 scene (80 objects, 7 lights).

Or via pin_scene.py (verifies byte-identity):

```sh
python3 ../../tools/pin_scene.py CHASE.LWS ../../Runtime/SCENES/CHASE.FLD --legacy-vlum \
  --pick Ship1.lwo=$(pwd)/Ship1.lwo \
  --pick big_m.lwo=$(pwd)/big_m.lwo \
  --pick m1.lwo=$(pwd)/m1.lwo \
  --pick m2.lwo=$(pwd)/m2.lwo \
  --pick m3.lwo=$(pwd)/m3.lwo \
  --pick m4.lwo=$(pwd)/m4.lwo \
  --pick m5.lwo=$(pwd)/m5.lwo \
  --pick mm7.lwo=$(pwd)/mm7.lwo
```

## Files

| File | Role | Source in Original/ |
|------|------|---------------------|
| `CHASE.LWS` | scene (camera/light/object tracks, 18 camera keys, 7 lights) | `Original/dos-rev/REVIVAL/SCENES/CHASING/CHASE.LWS` |
| `Ship1.lwo` | high-poly spaceship 1 (423 KB, s1_side/s1_up textures) | `Original/dos-rev/REVIVAL/SCENES/CITY/SHIP1.LWO` |
| `ship2.lwo` | spaceship 2 | `Original/dos-rev/REVIVAL/SCENES/CHASING/SHIP2.LWO` |
| `big_m.lwo` | big mountain (3 surfaces, F_surf.jpg texture, YAxis flag) | `Original/dos-rev/REVIVAL/SCENES/CHASING/BIG_M.LWO` |
| `m1.lwo`–`m5.lwo` | mountain tiles (Mount.jpg texture, XAxis flag) | `Original/dos-rev/REVIVAL/SCENES/CHASING/M*.LWO` |
| `mm7.lwo` | mountain tile variant | `Original/dos-rev/REVIVAL/SCENES/CHASING/MM7.LWO` |
| `water.lwo` | water plane (surface name "water") | `Original/Scenes/CITY/WATER.LWO` (394 bytes, default) |

## Archaeology notes

The key LWS version is `dos-rev/REVIVAL/SCENES/CHASING/CHASE.LWS` (18 camera keys), NOT
the `Original/Scenes/CITY/2/CHASE.LWS` (17 keys, gives -56 bytes = 1 missing keyframe)
nor the `Original/Scenes/CITY/CHASING/CHASE.LWS` (83 obj refs = 3 extra m2 instances).

The `Ship1.lwo` must be the **dos-rev CITY** variant (423 KB, `s1_side.jpg`/`s1_up.jpg`
texture names, `spc1 front glass` COLR=(156,162,175)), not the dos-rev CHASING variant
(same size but different surface colors for `spc1 front glass`).

Mountain objects (m1–m5, big_m) must be the dos-rev variants, which have `TFLG=XAxis`
(0x21/0x22 per surface). The `Original/Scenes/CITY/CHASING/*.LWO` variants have `TFLG=YAxis`
which produces byte-different FLD output.

### Legacy VLUM converter

The shipping `CHASE.FLD` was built before commit `b441da6` ("greets: fix LWO luminosity
units"), which changed `ReadSurfaceLuminosityFloat` from `Temp*100.0f` to `Temp`. The 1998
converter stored VLUM as a percentage, so Ship1's `in engine` surface (VLUM=1.0) was stored
as `Luminosity=100.0` in the FLD. Use `lwsread_legacy` (compiled with `-DLEGACY_VLUM=1`)
or `pin_scene.py --legacy-vlum` to reproduce the byte-identical result.
