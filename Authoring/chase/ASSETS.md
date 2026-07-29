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

### Regenerate lighthouse.lwo from the OBJ

```sh
tools/obj2lwo.py <lighthouse_tower.obj> Authoring/chase/lighthouse.lwo --recenter \
  --map "Material.007=tower,216,217,224,0.0025,1.0,LH_TOWER.JPG" \
  --map "Material.008=lamp,206,224,246,0.013,0.0,LH_LAMP.JPG" \
  --map "Material.010=door,46,49,60,0.0005,0.8,LH_DOOR.JPG"
```

The three `--map` entries assign each OBJ material a Flood surface with a solid
colour + a small solid-colour texture in `Runtime/TEXTURES` (`LH_TOWER.JPG`
whitish body, `LH_LAMP.JPG` cool-white lamp lens, `LH_DOOR.JPG` dark door).
A **texture is mandatory** — the deferred surface kernel skips untextured
materials (DeferredSurfaceKernel.cpp:1512), so a flat-colour-only surface would
render invisible. The lamp surface's emissive (VLUM) makes the lamp room read as
the light source; its local-Y centre is read back by `chase_lights.py` and used
as the swept-beam origin, so swapping in a different mesh needs no re-measuring.

The brick textures from the original .blend were not recoverable; the flat
white/cool/dark scheme was chosen for a clean read at chase distances.

## beacon.lwo — procedural fallback tower

`tools/chase_lights.py --runway --runway-model proc` generates a simpler faceted
tower (`beacon.lwo`) from parameters instead of the downloaded mesh. Kept as a
no-dependency fallback / A-B; the real lighthouse is the default look.
