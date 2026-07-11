# city — scene sources (FULLY PINNED: byte-identical regeneration)

The city fly-through. Ships as `Runtime/SCENES/CITY.FLD` (977,234 bytes, FldVersion 0.113).

## TL;DR parity status

- **BYTE-PARITY ACHIEVED.** From this directory,
  `../../tools/lwsread/build/lwsread_legacy CITY1.LWS CITY.FLD` produces a file
  **byte-identical to the shipping `Runtime/SCENES/CITY.FLD`**
  (md5 `2a773c317ce14a53afe4561f34e112fa`).
- **Scene definition (`CITY1.LWS`)** is the vintage dos-rev scene, identified by
  its light set (`shp1 engine glow` ×4 among 30 lights) and validated by
  byte-identical reproduction of the vintage `city1.FLD` before the building
  recovery.
- **17 of 20 objects** are the vintage dos-rev LWOs (byte-identical FLD records).
- **b1.lwo / b3.lwo / b6.lwo are RECOVERED files, not vintage bytes.** The 1998
  high-poly building LWOs (b1 192v/298f, b3 364v/570f, b6 474v/796f) do not
  exist anywhere in either checkout or any archive (see "Archaeology notes").
  These three were reconstructed from the shipping FLD by
  `tools/fld2lwo/fld2lwo_city.py`: geometry (PNTS/POLS) and surfaces
  (SRFS/SURF) extracted from the FLD object records and re-encoded as LWO1,
  bit-exact against tools/lwsread's reader transforms. They convert
  byte-identically, which is the property the pipeline needs.

## Regenerate the shipping FLD

From this directory, with the converter built (see `tools/lwsread`):

```sh
../../tools/lwsread/build/lwsread_legacy CITY1.LWS CITY.FLD
cmp CITY.FLD ../../Runtime/SCENES/CITY.FLD   # byte-identical
```

Legacy-VLUM build is required (the shipping FLD predates the `b441da6`
luminosity-unit fix), same as chase/fountain.

## Files

Objects copied from `Original/dos-rev/REVIVAL/SCENES/CITY/` except b1/b3/b6 (recovered, see above) (the 1998 DOS build
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
| `b1.lwo` | building type 1, 192 v / 298 f (×21) — **recovered from FLD** | ✅ exact (recovered) |
| `b3.lwo` | building type 3 ('b7' surfaces), 364 v / 570 f (×11) — **recovered from FLD** | ✅ exact (recovered) |
| `b6.lwo` | building type 6, 474 v / 796 f (×3) — **recovered from FLD** | ✅ exact (recovered) |
| `water.lwo` | water plane, 4 v / 1 f | ✅ exact |
| `taxi.lwo` (×8), `bus.lwo` (×5), `car2.lwo` (×2), `poliece.lwo`, `bike.lwo`, `abulans.lwo` | vehicles | ✅ exact |
| `tra_frnt.lwo` (×3), `tra_vagn.lwo` (×9), `ll_pas.lwo` (×2), `lll_pas.lwo` (×2), `pas2.lwo` | train | ✅ exact |

Before recovery, the vintage low-poly b1/b3/b6 accounted *exactly* for the
210,280-byte / 7,210-vertex gap to the shipping FLD:
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
- **Exhaustive search for the 1998 high-poly files** (both checkouts incl.
  `~/work/revival/dos-build/`, 1026 LWOs parsed by embedded SRFS surface sets,
  plus ZIP/ARJ/ISO/tar.gz archive contents, plus candidate conversion tests
  through lwsread_legacy): nothing carries the b1/b3/b6 surface sets at high
  poly counts. Closest misses: `B7.LWO` (b7 surface set, 56p — older revision),
  `B3_F.LWO` (360p but a *different* building's surfaces). Conversion tests
  also proved the converter maps LWO points 1:1 to FLD verts (no seam
  splitting): B3_F 360p -> 360v.
- **Recovery method (b1/b3/b6).** `tools/fld2lwo/fld2lwo_city.py` walks the
  v0.113 FLD, takes each building's material records + vertex/face blobs, and
  emits LWO1 with bit-exact inverse transforms (BE floats; *100 fields inverted
  in float32 with round-trip assertion; SwapYZ in LWSREAD.CPP is a commented-out
  NO-OP, so vectors pass through in order — this is the trap that breaks naive
  converters). Validated by full-file byte-identity of the regen.
