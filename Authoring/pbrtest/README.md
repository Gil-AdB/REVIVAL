# pbrtest — PBR / env-reflection test scene

A deterministic material lab, not a demo part: flat checkered floor + two
walls, six UV-spheres in a row, a cube, three ranged point lights and a slow
2-key camera dolly (240 frames). Ships as `Runtime/SCENES/PBRTEST.FLD`.

Use it to eyeball / regression-test the deferred material pipeline:

- **left sphere triple** sweeps GLOSSINESS 16 / 64 / 256 at specular 60%
- **right sphere triple** sweeps REFLECTION 0 / 40 / 80 at gloss 256
  (the env-reflection bake gates on `Reflection > 0` — with `--env_refl`
  the two right spheres mirror the room, Fresnel-weighted)
- every surface references the neutral checker `Runtime/TEXTURES/PBRTEST.PNG`,
  so PBR map imports (editor or `--material-import`) have a texture to replace
- floor/walls take planar UVs, spheres/box cubic — UV-mapping knobs testable

Run it:

```sh
cd Runtime && ./DEMO --scene-pbrtest --env_refl                # native
./DEMO --scene-pbrtest --env_refl --snapshot=pbrtest@t=100,400 --out=/tmp/x
# editor: DEMO.html?editor&scene=pbrtest
```

## Regenerate

The sources here are themselves GENERATED — edit `tools/make_pbrtest.py`
(geometry/surface parameters) or edit the .lwo/.lws directly in LightWave,
then:

```sh
python3 tools/make_pbrtest.py        # only if regenerating from the script
cd Authoring/pbrtest
../../tools/lwsread/build/lwsread PBRTEST.LWS PBRTEST.FLD
cp PBRTEST.FLD ../../Runtime/SCENES/PBRTEST.FLD
```

(Use the CURRENT converter — this is a new scene, not a 1998 pin; no
legacy/ofir build needed.)

## Converter gotchas learned here (apply to ANY new authored scene)

- **`AmbientColor` + `AmbIntensity` are mandatory** in the LWS — FLDSAVE
  writes those envelopes unconditionally and segfaults on null without them.
- **`PreviewLastFrame` is what becomes the FLD's LastFrame** (not
  `LastFrame`) — omit it and every scene driver's PartTime collapses to 0,
  the tick exits before rendering, and the snapshot is black.
- **Write the float material twins (`VDIF`/`VSPC`/`VRFL`) after the int
  chunks** — the 1998 int readers are broken (they read 1 byte of the 2-byte
  big-endian value: DIFF 256 → diffuse ≈ 0 → pitch-black diffuse, only
  specular arcs render). LightWave always writes both; so must generators.
- **Cull-normal sign is path-dependent** — the ground grid needed flipped
  winding to face up; probe + flip per surface orientation.
- Point lights need **`LightRange`** or the FLD omnis default Range to 0 and
  contribute nothing in the deferred path.
