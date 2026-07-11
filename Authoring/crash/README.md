# crash — scene sources (FULLY PINNED: byte-identical regeneration)

The crash scene (a laptop, screen-on, tilting shut — the demo's "END" scene,
wired into the director as `Run_Crash`). Ships as `Runtime/SCENES/CRASH.FLD`
(2,154 bytes, FldVersion 0.113, md5 `4f8aac84d743918ef790a790d1e18150`).

## TL;DR parity status

- **BYTE-PARITY ACHIEVED.** From this directory,
  `../../tools/lwsread/build/lwsread CRASH.LWS CRASH.FLD` produces a file
  **byte-identical to the shipping `Runtime/SCENES/CRASH.FLD`**
  (md5 `4f8aac84d743918ef790a790d1e18150`).
- **Scene definition (`CRASH.LWS`)** is the vintage `END.LWS` verbatim (2 objects,
  1 light `Light`, 1 camera). Identified by exact byte-reproduction of the
  shipping header, light, camera, materials, and `lt_hull` geometry.
- **`lt_hull.lwo` is the vintage 1998 LWO** (byte-identical FLD record).
- **`lt_scr.lwo` is a RECOVERED file, not vintage bytes.** The shipping laptop
  screen is a newer revision (screen panel recessed by ΔZ = −1.5, 12 verts) that
  exists nowhere in either checkout (see "Archaeology notes"). It was
  reconstructed from the shipping FLD by `tools/fld2lwo/fld2lwo_crash.py` — the
  same bit-exact FLD→LWO1 route used for the city buildings. It converts
  byte-identically, which is the property the pipeline needs.

## Regenerate the shipping FLD

From this directory, with the converter built (see `tools/lwsread`):

```sh
../../tools/lwsread/build/lwsread CRASH.LWS CRASH.FLD
cmp CRASH.FLD ../../Runtime/SCENES/CRASH.FLD   # byte-identical
```

No texture JPGs are needed for conversion (surface image names live in the LWO
SURF chunks, not the JPGs). Either converter build works: **crash has no
luminosity/emissive surfaces, so the legacy-VLUM vs non-legacy distinction is
moot here** (both `lwsread` and `lwsread_legacy` produce the byte-identical
result). The **current** reader is required for a different reason — see the
LastFrame note below.

## Files

Objects and scene copied from `Original/dos-rev/REVIVAL/SCENES/END/` (the DOS
build tree; the demo's `Run_Crash` is the "END" laptop scene), except `lt_scr.lwo`
(recovered, see above).

| File | Role | Shipping match |
|------|------|----------------|
| `CRASH.LWS` | scene = vintage `END/END.LWS` verbatim (2 obj refs, 1 light, 1 camera) | ✅ header/light/camera/materials exact |
| `lt_hull.lwo` | laptop base/keyboard, 8 v / 6 f (`leptop hull`, `key board place`) | ✅ exact (vintage `END/LT_HULL.LWO`) |
| `lt_scr.lwo` | laptop screen, 20 v / 11 f (`screen`, `leptop hull`, `lap top flood sing`) — **recovered from FLD** | ✅ exact (recovered) |

Surfaces reference textures `black.jpg`, `Lap_col.jpg`, `flodd.jpg`,
`keyboard.jpg` (already in `Runtime/TEXTURES/`; the vintage originals live
beside the LWOs in `Original/.../END/`).

## The LastFrame-180 detail

`END.LWS` declares `LastFrame 60` **and** `PreviewLastFrame 180`. The current
`tools/lwsread` maps *both* keywords to the same handler (last one wins), so the
FLD's frame range comes out `(1, 180, 1)` — matching the shipping FLD. The
**vintage** `LWSREAD.EXE` only honored `LastFrame`, so its output
(`Original/.../END/TEST.FLD` == `END.FLD`, md5 `2ab9f1b16f344f7404844e266c8ff77c`)
carries `LastFrame 60`. This is why the shipping FLD must be regenerated with the
current reader, not the vintage one. (`CRASH.CPP` drives frames
`Start..End = 1..180`.)

## The recovered screen: exactly what differs

The vintage `END/LT_SCR.LWO` and the shipping screen differ only in the Z of the
12 screen-panel verts (the inner display + its back face); X and Y are identical:

| vert | shipping Z | vintage Z |
|------|-----------|-----------|
| v8–v15 (inner panel) | −1.5 | 0.0 |
| v16–v19 (back face)  | −0.5 | 1.0 |

i.e. the whole screen surface was pushed ΔZ = −1.5 back into the bezel — a
one-nudge authoring tweak made after the vintage LWO was last saved. Everything
else in the object (materials, faces, the 8 bezel verts) is byte-identical.

### Recovery method

`tools/fld2lwo/fld2lwo_crash.py` (a trivial retarget of `fld2lwo_city.py` to
`CRASH.FLD` / `lt_scr.lwo`) walks the v0.113 FLD, takes the object's material
records + vertex/face blobs, and emits LWO1 with bit-exact inverse transforms
(BE floats; SwapYZ in `LWSREAD.CPP` is a commented-out NO-OP, so vectors pass
through in order). Self-checks by simulating the reader on the emitted bytes and
comparing the resulting FLD record against the shipping blob. Rebuild with:

```sh
cd ../.. && python3 tools/fld2lwo/fld2lwo_crash.py   # writes /tmp/recovered_lt_scr.lwo
cp /tmp/recovered_lt_scr.lwo Authoring/crash/lt_scr.lwo
```

## Registry recommendation (for `tools/editor_server.py`)

The scene registry currently has `"crash": {"authoring": False}`. Promote it to a
pinned authoring scene:

```python
"crash":    {"authoring": True,  "dir": "crash",    "lws": "CRASH.LWS",            "legacy": True},
```

`legacy` is set for consistency with the same-era city/chase/fountain FLDs; it is
functionally a no-op for crash today (no VLUM surfaces), but guards the x100
convention if the editor ever adds an emissive to a crash surface. (This README
only *recommends* the change — it was left unapplied to avoid colliding with the
in-flight editor work on `editor_server.py`.)

## Archaeology notes

- **Where the sources live.** `Original/dos-rev/REVIVAL/SCENES/END/` holds
  `END.LWS`, `LT_HULL.LWO`, `LT_SCR.LWO`, the four texture JPGs, and the vintage
  reader's `TEST.FLD`. Identical copies exist in `Scenes/_archive/end-scene/END/`,
  the sibling `~/work/revival/` checkout, and its `dos-build/` runtimes — every
  `LT_SCR.LWO` in every non-worktree location is the **same** md5
  (`055949e64d0aa3a973186a274b57deac`), i.e. the vintage screen (Z 0/1). None
  carries the recessed shipping geometry.
- **Two vintage FLDs, neither is shipping.** `END.FLD` / `END/TEST.FLD`
  (2,154 B, md5 `2ab9f1b16f344f7404844e266c8ff77c`) is the vintage reader's
  output of this scene: `LastFrame 60`, screen at Z 0/1, and a few differing
  bytes in the Light's envelope (a reader-version artifact — the current reader
  reproduces the shipping Light bytes exactly from the unchanged `END.LWS`). A
  separate, older `Original/.../SCENES/CRASH.FLD` (2,075 B, md5
  `13dee7353103455308fb37deceed9182`) is an earlier cut (also 60 frames, screen
  Z 0/1) — 79 bytes smaller, not the shipping content.
- **Exhaustive search for the recessed-screen LWO.** Both checkouts (incl.
  `dos-build/`), every `LT_SCR.LWO` matched by embedded surface set
  (`screen` / `leptop hull` / `lap top flood sing`), and both vintage FLDs: none
  has the shipping Z = −1.5/−0.5 screen. Like the city b1/b3/b6 buildings, the
  shipped revision survives only inside the FLD, hence the recovery.
