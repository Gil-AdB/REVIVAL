# The co-planar guard for the PolyId shadow tap

**Status: default OFF, byte-null off, STOPPED AT THE GATE.** The ON arm changes
the greets look on the dev box by a lot (see §5). It is not adopted; the M5 run
and the user's eye decide.

Branch `rev-shadowguard`, worktree `/Users/gil-ad/work/rev-shadowguard`, off
`4ee0f5da`.

---

## 1. The mechanism, as read from the code

`ShadowMode::PolyId` is the production shadow test. Its implementation is
`CubeShadow_Tail` in `FDS/FILLERS/ShadowMap.h`, with three further copies of the
same predicate:

| site | file | serves |
|---|---|---|
| cube tap | `FDS/FILLERS/ShadowMap.h` `CubeShadow_Tail` | moving omnis, the `dynamicOnly` composite |
| static lightmap bake | `FDS/RENDER/LightmapBake.cpp` `SampleStaticCubeAtWorld` | **greets' static omnis** — the path the ceiling defect runs through |
| spot tap | `FDS/RENDER/DeferredSurfaceKernel.cpp` | 2-D spot maps |
| cached cube tail | `FDS/RENDER/CubeTapPrepass.h` `CubeShadow_SampleCached` | `--deferred_cube_prepass` (calls the same tail) |

The test, verbatim in behaviour:

```
receiverId = uint16(surfaceMatId)                 // the receiver's ShadowMatID
c          = ShadowTexId(closest packed word)     // the winning texel's stored id
occ += w   iff  c != 0 && c != receiverId
```

**There is no depth term and no bias in it at all.** `DeferredShadowSampling.h`
even passes `constBias = 0, slopeBias = 0` into the PolyId call, with the comment
that PolyId "skips bias arithmetic entirely … so PolyId mode pays nothing for
slope it never uses."

The Depth-mode arm, in the same function, is:

```
pixZenc = clamp(0xFF80 - int(lz * sm.zScale), 0, 0xFFFF)
biased  = pixZenc + constBias(512) + slopeBias(up to 4096)
occ += w  iff  biased < closest(o)
```

### Why the depth test passes where the polyId path shadows

This is the M5's own question and the code answers it directly.

Depth mode's `constBias + slopeBias` **is** the acne rescue, and it is
**ownership-blind**. A co-planar occluder's stored depth is within the bias of the
receiver's own depth, so `biased < closest` is false and the receiver reads LIT
**whoever wrote the texel**.

PolyId's only rescue is `storedId == receiverId`. The moment a *different-material*
co-planar surface wins the shadow-map texel, the receiver is hard-shadowed with
**zero depth separation from its own plane**. Nothing in the test can notice that
the "occluder" is the receiver's own surface under another owner.

Which co-planar surface wins is decided in `ShadowBarry::apply_exact`
(`FDS/FILLERS/ShadowMap.cpp`) by

```
p_mask &= Vec8ib(enc > z_existing);   // strictly greater; first writer keeps a tie
```

so **one quantum** of raster noise flips ownership. The M5's shadow maps diverge
from the dev box's on 41/48 static, 26/28 dynomni and 2/2 content-bearing dynmesh
maps (ledger `0a9d99d9ead8`).

### The sharpening the code forces on the hypothesis

The brief's hypothesis was "the stored **polyId** belongs to a co-planar
**neighbour**". Confirmed, with one correction that matters:

* the id is a **material/shadow-group id** (`Material::ShadowMatID`, else
  `uint16_t(ID+1)`), **not a per-polygon id**. Co-planar polygons of the *same*
  shadow group never fight — they match and the rescue holds.
* greets deliberately allocates **many** shadow groups: `[PIRAMID-SPLIT] total
  clusters: 2183` on this scene, per-plane-cluster, and
  `--greets_displace_shadow_planes` (default ON) keys each displaced facet by its
  parent authored plane precisely so that *different* walls get *different* ids.
  So greets is a scene with ~2183 chances for a co-planar id mismatch, by design.

All three M5 observations are explained without residue:

| observation | explanation |
|---|---|
| `--no-shadows` restores the ceiling | no test runs at all |
| `--no-shadow_polyid` restores it | Depth mode's bias is ownership-blind |
| `--light_rect_exact` / `--no-deferred_zcull` / `--no-greets_mirror` change nothing | none of them touch the shadow test |

**Hypothesis CONFIRMED.** No fix was built for a mechanism that is not there.

---

## 2. The guard

`--shadow_coplanar_guard` (bool, default 0). In every PolyId arm, a texel whose
stored id differs from the receiver's **stops occluding** when the stored depth is
within a band of the receiver's own depth:

```
occludes(t) = (id != 0) && (id != receiverId)
              && ( dz > N || (!oneSided && dz < -N) )      dz = storedZenc - pixZenc
```

* `--shadow_coplanar_guard_quanta` (int, default **-1**). `-1` = use the
  **depth-mode bias for this very pixel**, `--shadow_bias`(512) + the slope-scaled
  `--shadow_slope_bias` term. A value ≥ 0 pins a constant band, for A/B.
* `--shadow_coplanar_guard_onesided` (bool, default 0). OFF = **symmetric**
  band; ON = occlude only when `dz > N`, i.e. only when the depth test would
  also call it an occluder.

### Why the band is the depth bias and not 1–2 quanta

The brief proposed N = 1–2 quanta. **Measured: that rescues nothing.** Under the
forced-mismatch repro of §3, at cam A:

| constant N | ceiling band | |
|---|---|---|
| 2 | 82.8 / 63.4 / 47.6 | no effect |
| 8 | 82.8 / 63.4 / 47.6 | no effect |
| 64 | 82.8 / 63.4 / 47.6 | no effect |
| 512 | 82.8 / 63.5 / 47.6 | no effect |
| 4096 | 83.6 / 103.4 / 104.6 | starts to bite |

The deltas are **slope-sized, not quantum-sized**. The receiver's depth is sampled
at the *pixel's* exact position while the stored depth is the occluder's depth at
the *texel it won*; one texel of footprint × the depth slope is thousands of
quanta on a grazing surface. This is the same quantity `--shadow_slope_bias`
(1024, reaching 4096 at the 0.2 N·L clamp) exists to cover in depth mode. So the
principled band is the depth-mode bias, and that is the default.

**What N trades.** Larger N rescues more co-planar mismatch and costs more of a
genuine contact shadow: a real occluder standing within N quanta of the receiver
loses its last N quanta of shadow. Because the default band is slope-scaled it is
narrow where the surface faces the light (512) and wide where it grazes (up to
4608) — which is where contact shadows are least well resolved anyway.

### Why symmetric, not one-sided

The one-sided form is the natural "the depth test must also agree". **It
degenerates to depth mode**: measured at cam A it is `bc7a68cc…`, which is
`--no-shadow_polyid` **byte for byte**, with or without the forced mismatch. Once
"depth must agree" is required, the identity test contributes nothing. So the
one-sided arm buys nothing over the flag that already exists, and the symmetric
band is the actual guard: it cannot un-shadow a genuine occluder standing off the
surface and it cannot un-shadow a surface *behind* the receiver.

### The two other things the guard has to touch

1. **The 8×8 uniformity pyramid's uniform-OCCLUDING fast path** returns `0.0f`
   without ever reading a depth, so it cannot be guarded. With the guard on it
   falls through to the real tap (`uniC = kShadowUniMixed`). The uniform-**LIT**
   fast path is kept — the guard only ever turns occluded into lit.
2. **`--deferred_cube_prepass` is disarmed** while the guard is on.
   `CubeShadow_SampleCached` is called with `lz = 0.0f` on purpose (in PolyId mode
   the depth arm is unreachable, so the prologue never stores lz). Feeding the
   guard `lz = 0` makes the receiver read as if it were *at the light* and
   silently turns the guard into "nothing ever occludes". **This bit me: my first
   one-sided result looked like a clean fix and was actually `--no-shadows`.**
   `CubeProTileArmed()` now returns false under the guard. That costs ~2.5 ms (§5).

---

## 3. Local reproduction of the M5 condition

The M5 is not reachable from here, so the ownership mismatch is forced locally.

**`--shadow_coplanar_flip=K`** (int, default 0) — ShadowBarry's depth test becomes
`enc + K > z_existing`, so the **last** writer wins every contest decided by fewer
than K quanta. This is the raster-noise lever. Measured at cam A: K=1 (exact ties
only) moves **3 pixels**; K=2, 4, 8 move 406 px at most, in a 250×160 box at
x[777,1030] y[400,560], and **never touch the ceiling band**. Conclusion: exact and
near-exact ownership ties are rare in these maps, so this lever cannot reach the
M5's state on its own. Kept because it is the honest measurement of how rare they
are.

**`--shadow_coplanar_idshift=N`** (int, default 0) — ShadowBarry stamps
`uint16_t(idByte + N)` into the id half, leaving the **depth half untouched** and
leaving the receiver's id alone. Every surface's own texels now carry an id that is
not the receiver's, so the identity rescue fails frame-wide — which is exactly the
state the M5's ceiling texels are in — while the stored depth is still the
receiver's own surface depth at that texel, the *definitional* co-planar occluder.
Use a shift clear of the ~2183 ids greets allocates (32768).

---

## 4. The causal A/B

greets cam A t=5965, 1920×1080, dev box (M2 Max), clean Release build.

```
CAM=22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499
BASE="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao \
      --greets-displace --force_xres=1920 --force_yres=1080"
```

| # | arm | md5 | ceiling band R G B |
|---|---|---|---|
| 1 | OFF (shipping) | `d92cb6f5eb19da4301588ae83af6a56e` | 85.2 133.5 139.0 |
| 2 | `--shadow_coplanar_idshift=32768` | `83d7065a654b5cb45cab7d00a9b94281` | **82.8 63.4 47.6** |
| 3 | 2 + `--shadow_coplanar_guard` | `8dbbafb77629598e5a3e2b1bfaae9e1a` | **85.1 133.8 139.3** |
| 4 | 2 + guard + `--shadow_coplanar_guard_onesided` | `bc7a68cc2e5eeec1ca4ab8ef350b5658` | 85.2 134.4 139.9 |
| 5 | `--shadow_coplanar_guard` alone | `749dbdcef7785525061b5b10f07d04be` | 85.3 133.8 139.3 |
| 6 | guard + onesided alone | `ce4f5dfe7f688da99f87631c4c66d63f` | 85.4 134.4 139.9 |
| 7 | `--no-shadow_polyid` | `bc7a68cc2e5eeec1ca4ab8ef350b5658` | 85.2 134.4 139.9 |
| 8 | `--no-shadows` | `35d62d66f50505e568cd04348992d9e4` | 86.5 134.7 140.0 |
| — | **M5 baseline (his run)** | — | **85.3 65.8 47.8** |
| — | M5 `--no-shadows` / `--no-shadow_polyid` | — | 86.5 134.8 140.0 / 85.4 134.4 140.0 |

**Row 2 is the M5's ceiling reproduced on a machine whose shadow maps are correct**
(82.8/63.4/47.6 against his 85.3/65.8/47.8). **Row 3 is the guard restoring it**
(85.1/133.8/139.3 against the correct 85.2/133.5/139.0) — with *every* identity
rescue in the scene destroyed, the guard alone carries the frame back to within
0.3/255 of the shipping look. Row 4 = row 7 byte for byte is the one-sided
degeneracy noted above.

Image: `docs/img/shadowguard/repro_idshift_camA_t5965_unguarded_guarded_diff.png`
(unguarded | guarded | 4× diff).

---

## 5. Gates

### Flag OFF, dev box, this tip — all byte-null

| gate | result |
|---|---|
| `tools/render_gate.sh` | **4/4 PASS** — mirrortest `4ac809e5…`, rttslot `826c09e6…`, conetest `b41894f9…`, halotest `166fa25a…` |
| greets acceptance cam A t=5965 | **`d92cb6f5eb19da4301588ae83af6a56e`** — the committed pin |
| `[SPH]` stream, `--shadow_plane_hash` | **byte-identical** to `docs/data/sph_dev_t5965.txt` |
| `--no-deferred_cube_prepass` alone | also `d92cb6f5…`, so the prepass disable is byte-null on its own and the whole ON-arm change below is the guard |

### Flag ON, dev box — measured, not assumed

`tools/render_gate.sh` under `FDS_SHADOW_COPLANAR_GUARD=1`: **4/4 PASS, all four
rows byte-identical**. Those scenes have no co-planar id conflict.

greets does. Same BASE recipe, default camera, OFF → ON:

| pose | md5 OFF → ON | px differing | px >8 | max | signed mean | brighter / darker |
|---|---|---|---|---|---|---|
| t=1588 | `55f51bce…` → `334aa3bc…` | 789,885 (38.09 %) | 155,842 | 120 | **+3.69** | 789,679 / 201 |
| t=2845 | `84e18552…` → `80f5269f…` | 558,382 (26.93 %) | 292,069 | 140 | **+12.07** | 556,760 / 1,585 |
| t=5965 | `3a953d4f…` → `b51c877c…` | 304,548 (14.69 %) | 99,823 | 138 | **+4.75** | 303,071 / 1,464 |
| cam A t=5965 | `d92cb6f5…` → `749dbdce…` | 413,078 (19.92 %) | 124,786 | 136 | **+4.63** | 412,590 / 477 |

cam A by band: top third 18,585 px >8 (+7.90), middle 26,445 (+12.75), bottom
(floor) 79,756 (+9.90).

**Direction: ≥99.6 % of every changed pixel is BRIGHTER.** The guard removes
shadow and never adds any, which is what it is built to do — but on this scene it
removes a great deal of it. Whether what it removes is wrongly-cast self-shadow
(correct) or real contact shadow (a look regression) is a judgement for the eye,
not for these counters.

Images: `docs/img/shadowguard/guard_camA_t5965_off_on_diff.png`,
`docs/img/shadowguard/guard_t2845_off_on_diff.png` (OFF | ON | 4× diff).

### Perf, flag ON (single run, `--deferred_prof=1`, cam A t=5965)

| phase | OFF | ON | Δ |
|---|---|---|---|
| `DeferredLighting-call` | 22.133 ms | 24.697 ms | **+2.56 ms (+11.6 %)** |
| `lighting-w1` | 17.503 ms | 20.002 ms | +2.50 ms (+14.3 %) |
| `lighting-w2` | 2.549 ms | 2.645 ms | +0.10 ms |

Single run, not a pinned measurement. Most of it is the disarmed
`--deferred_cube_prepass`. **+2.5 ms is not free** — if the guard is ever adopted,
the prepass slot needs an `lz` field so the fast path survives.

---

## 6. What to run on the M5

Same recipe as `tools/m5_diag4.sh`, from the repo root of a build of this branch:

```sh
cd /Users/gil-ad/work/rev-shadowguard && \
cmake -S . -B build -G Ninja -DMODPLAYER_DIR=/Users/gil-ad/work/revival-fog/Modplayer/modplayer && \
cmake --build build && cd Runtime && \
CAM="22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499" && \
BASE="--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --force_xres=1920 --force_yres=1080" && \
for ARM in "" "--shadow_coplanar_guard"; do \
  rm -rf /tmp/m5sg; \
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="$CAM" \
    ./DEMO $BASE $ARM --snapshot=greets@t=5965 --out=/tmp/m5sg >/dev/null 2>&1; \
  echo -n "[${ARM:-baseline}] $(md5 -q /tmp/m5sg/greets_t005965_color.ppm) "; \
  python3 -c "import numpy as np,sys;f=open('/tmp/m5sg/greets_t005965_color.ppm','rb');f.readline();l=f.readline();
w,h=map(int,l.split());f.readline();a=np.frombuffer(f.read(w*h*3),dtype=np.uint8).reshape(h,w,3);
print('%.1f %.1f %.1f'%tuple(a[5:40,600:1300].reshape(-1,3).mean(axis=0)))"; \
done
```

**The number that decides it.** The M5 baseline ceiling band is
`85.3 65.8 47.8`. If `--shadow_coplanar_guard` moves it to roughly
`85 134 139`, the guard fixes the M5 defect by the mechanism this document
claims. If it does not move, the M5's stored occluder is **not** at the receiver's
depth and the mechanism is something else — and this whole branch is refuted.

Then the eye: the same flag on a fly-through, against the dev-box images in §5.
The default stays OFF either way until he says otherwise.
