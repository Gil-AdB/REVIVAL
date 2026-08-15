# water round — running notes (do not trust memory, trust this file)

## Census (--water_census, new flag), per frame, 1920x1080
| pass | scene | scanned | rej_near(above horizon) | rej_far | rej_occl | LIVE | live% |
|---|---|--:|--:|--:|--:|--:|--:|
| ripple       | city t=1961  | 2073600 | 385920 (18.6%) | - (no far cut) | - (no occl test) | 1687680 | 81.39% |
| glints       | city t=1961  | 2073600 | 385920 (18.6%) | 109440 (5.3%) | 652958 (31.5%) | 925282 | 44.62% |
| glintsVaried | chase t=800  | 2073600 | 837120 (40.4%) | 15360 (0.7%)  | 103222 (5.0%)  | 1117898 | 53.91% |

GREETS HAS NO WATER AT ALL — no pwater:: calls, no "water" in GREETS.FLD. The
brief's "greets water ceiling" premise is false. Census point.

## PARENT baseline (d7a62231 + instrument only), wall_min ms / Ginstr / Gcyc
- chase t=800  water-glints  13.93 / 1.285 / 0.344
- city  t=1961 water-glints   7.54 / 0.545 / 0.175
- city  t=1961 water-ripple    4.19 / 0.428 / 0.109
- city  t=1961 frame_min 76.57 ; renderFrame 54.49

## STAGE 1 LANDED — runRowBands dynamic row-chunk dispatcher (bit-exact by construction)
- chase glints 13.93 -> 10.15 (-27.1%), effPar 11.1/12
- city  glints  7.54 ->  5.00 (-33.7%), effPar 11.0/12
- city  ripple  4.19 ->  3.05 (-27.2%), effPar 10.9/12
- city frame_min 76.57 -> 71.51
- Gcyc essentially FLAT (chase .344->.346, city .175->.170) => pure parallelism win.
- Strided-single-row probe was WORSE than chunked (chase 12.37 vs 10.15) — kept chunks of 8.

## Ablations on top of stage 1 (price only, not shipped as-is)
- A: 4 swell cos/sin in waterWaveSlopeVaried -> linear
    chase 10.15 -> 8.96 (-11.7%), Ginstr 1.172 -> 0.936 (-20.1%), Gcyc .346 -> .306 (-11.6%)
    => the 4 transcendentals are worth ~1.19 ms of chase's glint pass
- B: std::pow(ndh,shin) -> ndh*ndh
    chase 10.15 -> 9.58 (-5.6%),  Ginstr 1.172 -> 1.098, Gcyc .346 -> .325 (-6.1%)
    city   5.00 -> 4.24 (-15.2%), Ginstr 0.481 -> 0.426, Gcyc .170 -> .144 (-15.3%)
    => powf worth ~0.57 ms chase, ~0.76 ms city

## Disassembly facts
- band_plain  (RenderGlints  $_1) 367 instrs: 5 fdiv, 3 fsqrt, bl powf, bl waterWaveSlope (NOT inlined)
- band_varied (RenderGlintsVaried $_1) 919 instrs: 9 fdiv, 3 fsqrt, bl cosf x3, bl sinf, bl powf
- NO sdiv anywhere: the `% W` in sampleWaterTex is already a cmp+csel (clang range-derived i0<W)
- 6 of band_varied's 9 fdiv come from sampleWaterTex's RUNTIME g_waterTexW/H (u/W, v/H per tap x3)

## THREE-ARM INTERLEAVED min-of-6 (r0 dropped, load 19->17), one asset tree
par = d7a62231 + instrument only (DEMO_ins); chi = + runRowBands + ndhMin (DEMO_chi)

| item | par | chi | delta |
|---|--:|--:|--:|
| chase t=800 water-glints | 14.195 | 10.021 | -4.174 ms (-29.4%) |
| city t=1961 water-glints |  7.720 |  4.602 | -3.118 ms (-40.4%) |
| city t=1961 water-ripple |  4.015 |  3.117 | -0.898 ms (-22.4%) |
| city FRAME_MIN           | 76.960 | 73.410 | -3.550 ms (-4.6%) |
| city ANIM                |  4.061 |  3.176 | -0.885 |

ATTRIBUTION CHECK (controls unmoved):
  renderFrame Ginstr chase 3.728 vs 3.732, city 6.057 vs 6.056 (3 decimals)
  gbuffer / DeferredLighting-call wall + Ginstr flat on both scenes.
Ginstr: chase glints 1.268->1.128 (-11.0%), city glints .523->.434 (-17.0%), ripple .424->.406
Gcyc:   chase glints .342->.333 (-2.6%),   city glints .171->.151 (-11.7%), ripple .108->.107
=> chase's win is almost PURE PARALLELISM; city's is parallelism + the powf early-out.

GATES: all eight pins reproduce their RECORDED values, par 2/2 and chi 3/3
       (chase 7678a6bc/42d79fad/b29c73f1/31aa5203/1544b0e7, city 3f894823,
        fountain 8db68ccb, greets 570a7b44). render_gate 4/4 PASS. BIT-EXACT.

## KILLED, with numbers (all bit-exact, all measured flat)
- L1 constant texture dims (5 fdiv/px removed from the varied band): Ginstr
  1.172->1.181, Gcyc .346->.340. The fdivs are independent per-tap and the OoO
  core hides them entirely. KILL.
- L2 occlusion test hoisted above the wx/wz reconstruction (fires on 652 958
  px/frame in city): ~10 instr x 652 958 = 6.5 M of 477 M = 1.4%. Sub-noise. KILL.
- L3 per-ROW horizon/far early-out (436 of 1080 rows chase, 201+57 city, all
  exactly row-aligned): ~10 instr x 837 120 = 8.4 M of 1 172 M = 0.7%. KILL.
  THE REASON ALL THREE DIE: the reject path is ~10 instructions against
  ~1050 INSTRUCTIONS PER LIVE PIXEL. The scan is not the cost; the shading is.
