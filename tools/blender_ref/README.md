# External displacement reference (Blender)

An independent second implementation of the greets stone displacement: Blender 4.5 (Cycles, as the
`bpy` Python module) displaces the exact mesh the engine bakes, with the same height map at the bake's
mip, the same amplitude and mid-level, along the smooth vertex normal, seen through the engine's own
camera. It exists to show what a correct junction looks like at the review poses, and to diff the
in-tree bake and the in-tree reference renderer against something that was not written here.

Findings and the four-pose deck: `docs/SESSION_STATE.md` (2026-09-03b), evidence in
`docs/evidence/blender_ref/`, ledger `groundwork query greets.displace.external_ref`.

## Files

- `ref.py` — the bpy script. Imports the OBJ, maps FDS (x,y,z) → Blender (x,z,y) and flips winding,
  optionally welds (`welded`) and splits T-junctions (`tfix`), sets up the camera from the engine's
  `Kick_Camera` basis, displaces (Displace modifier, or Cycles adaptive micro-displacement with
  `--adaptive`), renders, optionally exports the displaced mesh.
- `corr2.py` — high-pass luminance correlation of a Blender PNG against an FDS colour dump at unit
  scale (whole frame + a crop box). Calibration check; the wrong-pose control reads 0.
- `tjunction_census.py` — the T-junction census of an engine OBJ dump (a welded vertex lying strictly
  inside another face's edge), grouped by host line.

## Recipe

```sh
# 1. dump the authored stone mesh (byte-null flag; writes into Runtime/)
cd /Users/gil-ad/work/revival-fog/Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
../build/DEMO/DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --greets_mesh_dump \
  --force_xres=1920 --force_yres=1080 --snapshot=greets@t=6194 --out=/tmp/meshdump

# 2. the height map at the bake's mip (1024 -> 256, box filter)
python3 -c "from PIL import Image; Image.open('/Users/gil-ad/work/revival-fog/Runtime/TEXTURES/greets_wall_h.png').convert('L').resize((256,256),Image.BOX).save('/tmp/greets_wall_h_mip2.png')"

# 3. Blender as a Python module (once; python 3.11)
cd /Users/gil-ad/work/revival-fog/tools/blender_ref && python3.11 -m venv venv && ./venv/bin/pip install "bpy==4.5.*" numpy pillow scipy

# 4. one pose, one arm
./venv/bin/python ref.py --obj /Users/gil-ad/work/revival-fog/Runtime/greets_stone_authored.obj \
  --height /tmp/greets_wall_h_mip2.png --albedo /Users/gil-ad/work/revival-fog/Runtime/TEXTURES/greets_wall.png \
  --cam="22.4811096,5.24028063,-63.2136497,-0.996247888,-0.0673760772,-0.0543192849" --fov 58.1092 --pax 1 --pay 1.3333333 \
  --amp 0.3 --mid 0.547 --samples 48 --sun "0.45,0.6,0.35" --adaptive 1.0 --arm tfix --look tex --out /tmp/r_h6194_tfix_tex
```

Poses used on 2026-09-03 (`--cam=` with the equals sign: a leading minus is otherwise read as an option):

| pose   | `--cam`                                                                         | FDS t |
|--------|----------------------------------------------------------------------------------|-------|
| H6194  | `22.4811096,5.24028063,-63.2136497,-0.996247888,-0.0673760772,-0.0543192849`     | 6194  |
| H5981  | `22.1861649,3.2701385,-60.408123,-0.911465347,-0.0710642338,0.405193508`         | 5981  |
| corner | `-9.2,2.6,-53.2,-0.730,-0.05,-0.684`                                             | 6194  |
| line5  | `9.8,5.0,-21.3,0.812,-0.03,-0.583`                                               | 6194  |

## Camera traps (settled empirically, see the deck)

- The engine's `PerspY = 0.75 · PerspX` at 1920×1080 (AspectRatio 4/3 × 1080/1920). Blender clamps
  `pixel_aspect_x` to ≥ 1, so `--pax 0.75` is silently 1; use `--pax 1 --pay 1.3333333`. Verified by a
  vertical-scale sweep of the correlation (peak at 1.00 only with the right aspect).
- FOV: `--fov` is the engine's full horizontal FOV in degrees (`[GREETS-CAM] applied pose … fov=` in the
  DEMO log); Blender `sensor_fit HORIZONTAL`, `angle = fov`.
- Height sampling: the engine reads v top-down (row = v·h); the OBJ dump writes `vt = (u, 1−v)` so
  Blender's bottom-up convention samples the same texel.
