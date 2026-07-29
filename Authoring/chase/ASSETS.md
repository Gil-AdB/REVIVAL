# chase — third-party assets

## lighthouse.lwo — beacon tower for the runway light-drama

The runway variant (`tools/chase_lights.py --runway`) plants a real lighthouse
mesh at each beam emitter (see `README.md` and the tool's docstring).

- **Source:** "Low-Poly Lighthouse Scene" by **ultr4vis1tor**
- **URL:** https://opengameart.org/content/low-poly-lighthouse-scene
- **License:** CC0 (public domain — credit appreciated, not required)
- **Provenance:** the `lighthouse_tower` object from that scene (670 tris),
  exported to OBJ, converted to LWO1 by `tools/obj2lwo.py` (recentred so the
  base sits at Y=0 and the X/Z footprint is on the origin; native scale kept).

### Regenerate lighthouse.lwo from the OBJ (red/white striped)

```sh
tools/obj2lwo.py <lighthouse_tower.obj> Authoring/chase/lighthouse.lwo --recenter \
  --map "Material.007=tower,226,224,220,0.0012,1.0,LH_STRIPE.JPG,proj=planar,axis=x,tsiz=100:30.67:100,tctr=0:15.34:0" \
  --map "Material.008=lamp,255,244,214,0.013,0.0,LH_LAMP.JPG" \
  --map "Material.010=door,46,49,60,0.0005,0.8,LH_DOOR.JPG"
```

The tower body carries `LH_STRIPE.JPG` — horizontal red/white lighthouse bands
(red dome, red band under the lamp) — via a **Planar-X projection sized to the
mesh height** (`Get_UV`: texture V = -(localY - tctr.y)/tsiz.y + 0.5, so texture
rows follow the tower height; a stripe texture is U-uniform, so the planar seam
is invisible). `LH_LAMP.JPG` is the warm-white lamp lens, `LH_DOOR.JPG` the dark
door. A **texture is mandatory** — the deferred surface kernel skips untextured
materials (DeferredSurfaceKernel.cpp:1512), so a flat-colour-only surface would
render invisible. Keep the tower's VLUM tiny: for TEXTURED surfaces the emissive
is colourless (`Luminosity*255`), so a big value washes the red stripes pink.
The lamp surface's emissive makes the lamp room read as the light source; its
local-Y centre is read back by `chase_lights.py` as the swept-beam origin, so
swapping in a different mesh needs no re-measuring.

The brick textures from the original .blend were not recoverable; the striped
scheme is generated (see the `LH_STRIPE` block in the tools history) and reads
as "lighthouse" at chase distances far better than flat white.

## beacon.lwo — procedural fallback tower

`tools/chase_lights.py --runway --runway-model proc` generates a simpler faceted
tower (`beacon.lwo`) from parameters instead of the downloaded mesh. Kept as a
no-dependency fallback / A-B; the real lighthouse is the default look.
