# fountain-extended — the 2018–2020 revival fountain

The "extended" fountain cut that shipped in the revival from `f65ac32` (2018-12)
through `0bf67d4` (2020-11): the base fountain **plus sky dome, spaceship 1 and
six sht1 shuttles** — 25 unique materials, 8 lights, 140,988 bytes. It was
replaced at `bfb9c1c` (2020-11) by the current 512 KB `FOUNTAIN.FLD`
(= `Authoring/fountain/`, which adds ship 2 + cockpit and goes to 14 lights).

The FLD itself is not checked in — recover it any time with:

```sh
git cat-file blob 0bf67d4:Runtime/SCENES/FOUNTAIN.FLD > FOUNTAIN-EXT.FLD
```

## Regenerate the FLD

This scene needs the **OFIR-generation** converter build (see below), not
`lwsread` or `lwsread_legacy`. From this directory:

```sh
cmake -S ../../tools/lwsread -B ../../tools/lwsread/build && cmake --build ../../tools/lwsread/build
../../tools/lwsread/build/lwsread_ofir "FOUNTA~1.LWS" FOUNTAIN-EXT.FLD
```

Or verify byte-identity in one shot:

```sh
git cat-file blob 0bf67d4:Runtime/SCENES/FOUNTAIN.FLD > /tmp/fountain_extended.fld
python3 ../../tools/pin_scene.py "FOUNTA~1.LWS" /tmp/fountain_extended.fld --ofir \
  --pick moutine.lwo=$(pwd)/moutine.lwo --pick fount.lwo=$(pwd)/fount.lwo \
  --pick sky.lwo=$(pwd)/sky.lwo --pick SHIP1.lwo=$(pwd)/SHIP1.lwo \
  --pick inbal.lwo=$(pwd)/inbal.lwo --pick dio.lwo=$(pwd)/dio.lwo
```

## Files

| File | Role | Source in Original/ |
|------|------|---------------------|
| `FOUNTA~1.LWS` | scene (22 obj refs, 8 lights, DOS 8.3 name kept verbatim) | `Original/Scenes/CITY/FOUNTAIN/FOUNTA~1.LWS` |
| `moutine.lwo` | mountain tile (337v/532f — NOT the S1FNT 155v variant) | `Original/Scenes/CITY/FOUNTAIN/MOUTINE.LWO` |
| `fount.lwo` | fountain geometry (1102v/1662f — NOT the S1FNT 1000v variant) | `Original/Scenes/CITY/FOUNTAIN/FOUNT.LWO` |
| `sky.lwo` | sky dome (162v/320f, of 7 candidate copies) | `Original/Scenes/CITY/FOUNTAIN/SKY.LWO` |
| `SHIP1.lwo` | LOW-poly spaceship 1 (1024v/1424f, `s1_up.jpg`/`s1_side.jpg`) | `Original/Scenes/CITY/SHIP1.LWO` |
| `sht1.lwo` | shuttle (6 instances) | `Original/Scenes/CITY/FOUNTAIN/SHT1.LWO` |
| `inbal.lwo` | inbal object | `Original/Scenes/CITY/FOUNTAIN/INBAL.LWO` |
| `dio.lwo` | dio object | `Original/Scenes/CITY/FOUNTAIN/DIO.LWO` |

## Archaeology notes

- The correct LWS is `FOUNTA~1.LWS`, not its sibling `FOUNT.LWS`: the target
  FLD has FirstFrame 0 and a `Camera Target` null object in exactly
  `FOUNTA~1.LWS`'s object order; `FOUNT.LWS` writes FirstFrame 1.
- Every ambiguous object resolves to the `FOUNTAIN/` directory copies, and
  `SHIP1.lwo` is the **low-poly** `Original/Scenes/CITY/SHIP1.LWO` — not the
  423 KB dos-rev high-poly used by the final 512 KB fountain. The two were
  told apart by the texture-name strings inside the material records
  (`s1_up.jpg` vs `up.jpg`).

### OFIR-generation converter (`lwsread_ofir`)

With `lwsread_legacy` the pin lands 2 bytes short: header bytes 15–16 =
`LastFrame` (1477 vs 1600). The ported reader takes the FLD's LastFrame from
the LWS keyword `PreviewLastFrame` (1477 here), but the older-generation
reader in `Original/READERS/LWREAD/OFIR/LWSREAD.CPP` reads the `LastFrame`
keyword (1600 here) — so this FLD was converted by that OFIR-era tool.
`lwsread_ofir` = `LEGACY_VLUM` + `OFIR_LASTFRAME` (the one keyword-table
entry reverted) and reproduces the FLD byte-for-byte from the pristine,
unmodified sources above.
