# city — scene sources (partial: scene pinned, 3 building objects lower-poly)

The city fly-through. Ships as `Runtime/SCENES/CITY.FLD` (977,234 bytes, FldVersion 0.113).

## TL;DR parity status

- **Scene definition (`CITY1.LWS`): identified with high confidence.** Its object
  tree, instance counts, camera, materials and — decisively — its *light set*
  (`shp1 engine glow` ×4, `bilding flare` ×15, `big bilding flare` ×10, all engine
  lights) are byte-for-byte the same inventory the shipping FLD carries.
- **Objects: 17 of 20 match the shipping FLD exactly; 3 buildings do NOT.**
  `b1`, `b3`, `b6` shipped as **higher-poly** models than any `.lwo` recovered in
  either checkout or any archive (see table). Those three high-poly building
  objects appear to be **lost**.
- **Therefore this source set does NOT reproduce the shipping `CITY.FLD`.**
  Converting it yields the vintage **`city1.FLD`** (766,954 bytes) byte-identically
  — 210,280 bytes smaller, first difference at offset 0x538 (the first `b1`'s
  vertex count: 72 here vs 192 shipped).
- **Do not promote city to `authoring: True`.** Regenerating would swap in
  low-poly b1/b3/b6 (different silhouettes → different rasterized pixels) and
  break the city parity pin. City must stay FLD-patch mode until the high-poly
  b1/b3/b6 are recovered or re-modeled. See "Registry" below.

## Regenerate (produces vintage city1.FLD, NOT the shipping FLD)

From this directory, with the converter built (see `tools/lwsread`):

```sh
../../tools/lwsread/build/lwsread_legacy CITY1.LWS CITY.FLD
```

Produces a 766,954-byte, FldVersion-0.113 scene (110 objects, 30 lights, 1 camera,
frames 0..1401), **byte-identical to `Original/dos-rev/REVIVAL/SCENES/CITY/city1.FLD`**.
It is NOT byte-identical to the shipping `Runtime/SCENES/CITY.FLD`.

Legacy-VLUM build is required (shipping/vintage FLDs predate the `b441da6`
luminosity-unit fix), same as chase/fountain. Verify with:

```sh
python3 ../../tools/pin_scene.py CITY1.LWS \
  ../../Original/dos-rev/REVIVAL/SCENES/CITY/city1.FLD --legacy-vlum \
  --pick b1.lwo=$(pwd)/b1.lwo    --pick b2.lwo=$(pwd)/b2.lwo \
  --pick b3.lwo=$(pwd)/b3.lwo    --pick b4.lwo=$(pwd)/b4.lwo \
  --pick b5.lwo=$(pwd)/b5.lwo    --pick b6.lwo=$(pwd)/b6.lwo \
  --pick water.lwo=$(pwd)/water.lwo  --pick SHIP1.lwo=$(pwd)/SHIP1.lwo \
  --pick shp2.lwo=$(pwd)/shp2.lwo    # ... (all 20 objects; dos-rev CITY set)
```

## Files

All objects copied from `Original/dos-rev/REVIVAL/SCENES/CITY/` (the 1998 DOS build
tree that also holds the vintage `CITY.FLD`/`CITY-new.FLD`/`city1.FLD` and the
original `LWSREAD.EXE`). Named as `CITY1.LWS` references them (DOS paths stripped).

| File | Role | Shipping match |
|------|------|----------------|
| `CITY1.LWS` | scene (109 obj refs, 30 lights incl. 4× `shp1 engine glow`, 1 camera) | scene inventory ✅ |
| `SHIP1.lwo` | hero spaceship, 18,072 verts / 17,088 faces (423 KB) | ✅ exact |
| `shp2.lwo` | second ship, 192 v / 298 f | ✅ exact |
| `b2.lwo` | building type 2, 86 v / 89 f (×15) | ✅ exact |
| `b4.lwo` | building type 4, 106 v / 162 f (×11) | ✅ exact |
| `b5.lwo` | building type 5, 157 v / 216 f (×10) | ✅ exact |
| `b1.lwo` | building type 1, **72 v / 88 f (×21)** | ❌ ship = **192 v / 298 f** |
| `b3.lwo` | building type 3, **56 v / 66 f (×11)** | ❌ ship = **364 v / 570 f** |
| `b6.lwo` | building type 6, **40 v / 40 f (×3)** | ❌ ship = **474 v / 796 f** |
| `water.lwo` | water plane, 4 v / 1 f | ✅ exact |
| `taxi.lwo` (×8), `bus.lwo` (×5), `car2.lwo` (×2), `poliece.lwo`, `bike.lwo`, `abulans.lwo` | vehicles | ✅ exact |
| `tra_frnt.lwo` (×3), `tra_vagn.lwo` (×9), `ll_pas.lwo` (×2), `lll_pas.lwo` (×2), `pas2.lwo` | train | ✅ exact |

The entire 210,280-byte / +7,210-vertex / +12,222-face gap between this set and the
shipping FLD is accounted for *exactly* by b1/b3/b6:
`(192-72)×21 + (364-56)×11 + (474-40)×3 = 7,210` verts.

## Archaeology notes

- **Which LWS.** Three `CITY*.LWS` differ only in light count: `CITY_J~1.LWS`
  (0× `shp1 engine glow`), `CITY.LWS` (1×), `CITY1.LWS` (4×). The shipping FLD has
  **4×** → `CITY1.LWS`. Byte-check: `CITY1.LWS`→`city1.FLD` and `CITY.LWS`→`CITY-new.FLD`
  both reproduce byte-identically under `lwsread_legacy`.
- **Object variants.** The dos-rev CITY copies are the ones that reproduce the
  vintage FLDs. Other copies exist under `Original/Scenes/CITY/{,A/,CARS/,TRAIN/}`,
  `Original/READERS/LWREAD/D_CITY/`, and the sibling `~/work/revival/` checkout —
  none contains a 192-vert b1, 364-vert b3, or 474-vert b6. The closest is
  `Original/Scenes/CITY/{,A/}B3_F.LWO` at 360 verts (b3 shipped at 364 — not equal).
- **What's lost.** Only the high-detail b1/b3/b6 building meshes. Recovering them
  (or re-detailing b1/b3/b6 to 192/364/474 verts with identical surfaces) is the
  only path to a byte-identical `CITY.FLD` regeneration.
