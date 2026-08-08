# SHADING_CONTRACT.md — the CPU kernel and the GPU oracle, quantity by quantity

REVIVAL/FLOOD has two renderers that shade the same scene:

- the shipping **CPU software renderer** — `FDS/RENDER/DeferredSurfaceKernel.cpp`, a deferred
  G-buffer + PBR per-pixel kernel;
- a standalone **Metal deferred renderer** — `GpuBench/`, shaders in
  `GpuBench/shaders/deferred.metal`, used purely as a benchmark and a ground-truth instrument.

They share scene DATA (GpuBench links FDS and ingests the real scene, materials, lights, camera
splines and engine tangents) and share **no shading code, deliberately**. That independence is
what makes the GPU arm an oracle: a bug present in one is visible against the other. Merging them
would destroy exactly that property, because a shared bug is invisible to both sides. **The
decision is NOT to merge. Reuse stays at the DATA layer.**

What was missing is a way to verify agreement by **contract** rather than by eyeball. Every
divergence caught so far (`GPU_BENCHMARK_PLAN.md` §6.2b/§6.2d — a squared light colour, an
invented `FssEss/Fms` environment term, a surviving `OmniSizeMult`, a GGX lobe at roughness 0.625
where the CPU runs 0.2, a floor UV retile 1.5× too dense) was found by rendering whole frames and
comparing. That is slow, and it only catches what happens to be visible at the poses someone chose
to look at.

This document is the contract: every quantity in the lighting pipeline, CPU expression against GPU
expression, with citations. **Phase 1 fixes nothing.** It reports.

> Line numbers are anchors, not contracts — grep the symbol if it moved. Everything below is
> **read out of the source** at `fog-wt` / `212d48a` unless a row explicitly says MEASURED
> (a number someone ran) or INFERRED (a consequence I derived but did not run).

---

## 0. Which code paths this contract is about

| | CPU | GPU |
|---|---|---|
| entry | `Render_DeferredLighting` → `Render_DeferredLighting_Tile` (`DeferredSurfaceKernel.cpp:5441`, `:1341`) | `fs_gbuffer` + `fs_lighting` (`deferred.metal:161`, `:576`) |
| which branch | the **scalar** per-light loop (`:2264-2456`) | the only branch |
| why that branch | `deferred_vec` defaults **0 on arm64** (`FeatureFlags.h:27-31`), and greets' normal-mapped pixels force scalar anyway (`:1868-1869`) | — |
| shading model | `--pbr` Cook-Torrance, set by `GreetsApplyRunDefaults` (`GREETS.CPP:1107-1114`) | same, hard-coded |
| output | `g_hdrBuf` under `--hdr --hdr_linear` (`:2616-2636`) | `hdrTex` RGBA16Float |

Two other CPU kernels exist and are **out of scope here**: the 8-wide `..._Tile_OuterVec` (`:3926`,
off for greets by scene policy) and the transparent kernel (`:2752`, whose layers the GPU arm does
not implement at all). Where the OuterVec path differs from the scalar path in a way that matters,
the row says so.

**The comparable unit** is *material + geometry + light inputs → linear radiance in `g_hdrBuf` /
`hdrTex`*. It spans **one** CPU pass and **two** GPU passes, because the CPU fetches albedo /
normal / roughness / metal / AO **inside the lighting kernel** while the GPU fetches them in the
G-buffer pass. Any harness must bracket both GPU passes or it is comparing different things.

---

## 1. The two unit systems, and the composite — read this before the table

Almost every "is this off by 255 / by π / by 4?" question below resolves here.

**CPU** works in a **0–255 radiance scale**. The light list stores `colB = O->L.B * O->ISize` with
`O->L.B` the authored 0–255 byte (`:5551-5553`). The ambient SH is projected from a probe bake of
0–255 LDR face renders (`EnvBake.cpp:1737-1758`). The albedo texel is a 0–255 gamma byte. The
tonemap divides by 255 on the way out: `kE = exposure * (1/255)` (`Hdr.cpp:818-819`).

**GPU** works in a **0–1 scale**. Light colour is `L.color/255 * ISize` (`Deferred.mm:716-717`),
SH is projected from `.../255.0` (`Deferred.mm:263`), the tonemap takes `c * exposure` directly
(`deferred.metal:869`).

So the whole GPU frame should equal the CPU frame ÷ 255, and the tonemaps then agree. **That part
checks out.**

**The composite** — the single expression the whole HDR frame is built from — is CPU
`:2618-2636`:

```cpp
const float kN = 1.0f / 255.0f;
const float aB = texB*kN, aG = texG*kN, aR = texR*kN;
float rlB = aB*aB*lB + sB, ...;          // ← lB, not fdB
h[0] = fds::HdrClamp(rlB); ... h[3] = 1.0f;
```

Three things follow, and two of them are the source of most of §3:

1. **The ALBEDO is squared; the LIGHT is not.** `--hdr_linear` "square[s] the (normalized) albedo
   and let[s] light enter at power 1" (`:1374-1381`). The GPU does the same square at the point of
   use (`deferred.metal:383`, `S.baseColor = alb.rgb * alb.rgb`), with the G-buffer holding the raw
   gamma texel exactly as the CPU's does (`gAlbedo` is `MTLPixelFormatRGBA8Unorm`, **not** `_sRGB`
   — `Deferred.mm:502`). **AGREE.**
2. **The HDR write uses `lB`, the raw diffuse accumulator — NOT `fdB`.** Everything applied to
   `fdB` between `:2509` and `:2583` is applied **only to the LDR VPage combine** and is silently
   discarded in the HDR frame. That is documented for `--diffuse_energy`'s `(1-F)`
   (`GPU_BENCHMARK_PLAN.md` §6.2d) — but **the same is true of the metalness diffuse-kill**, which
   is not documented anywhere and which the GPU applies. See D1.
3. **`sB` (specular) enters the composite unmodified**, so everything applied to `sB` — roughness
   map, metal tint, env compose, `SpecMul` — *is* in the HDR frame.

---

## 2. The contract table

Verdicts: **AGREE** (same quantity by the same expression, modulo stated precision),
**DIVERGE** (different quantity — numeric consequence given where I could derive it),
**SCOPE** (deliberately absent on one side, already recorded in the plan),
**UNKNOWN** (I could not establish it from source alone).

### 2.1 Albedo, decode, linearisation

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| A1 | albedo fetch | point sample `texData[swizzledUV]` at the rasterizer-chosen mip (`:1594-1613`); `texture_filter` defaults 0 (`FeatureFlags.def:131`) | hardware trilinear + aniso, `albedoTex.sample(samp, in.uv)` (`:179`) | **SCOPE** — the mipmap-via-subdivision clipper is the CPU's substitute for a texture unit (plan §3). MEASURED contribution: ~⅓ of the whole-frame mean \|dY\| at t=5743 (§6.2e) |
| A2 | linearisation | `aB = texB/255; aB*aB` at the composite (`:2623-2625`) | `alb.rgb * alb.rgb` in `DecodeSurface` (`:383`) | **AGREE** |
| A3 | per-material tint | `texB *= Mat->TintB` etc. (`:1619`), default 1.0 (`Material.h:238-240`) | not ingested | **UNKNOWN** — inert if greets leaves all three at 1.0, which I did not verify per material. `grep TintB DEMO/GREETS.CPP` is empty, so the risk is an editor/RVSF-authored value |
| A4 | albedo alpha | `texA` carries baked AO under `Mat_AoInAlpha` (`:1588`, `:1856`) | `alb.a`, same role (`:190`) | **AGREE** |
| A5 | untextured materials | the kernel **`continue`s** on `!Mat->Txtr` (`:1527`) — the `BaseCol` branch at `:1765-1768` is unreachable in deferred | renders `b.baseColor` with `baseColor.w = 0` (`:178`, `Deferred.mm:869-871`) | **DIVERGE** — 3 of greets' 26 material bindings are untextured (plan §2.3). The CPU leaves those pixels unwritten (sky/backdrop shows through); the GPU shades them. Small area, but it is a real difference in *what gets drawn*, not just how |

### 2.2 Light colour, units, attenuation

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| L1 | light colour | `colB = O->L.B * O->ISize`, authored 0–255 treated **linear at power 1** (`:5551-5553`) | `color = (L.color/255) * L.intensity`, `intensity = O->ISize` (`Deferred.mm:716-717`, `SceneIngest.cpp:528`) | **AGREE** (this is the fixed §6.2d bug 1) |
| L2 | range cutoff | hard: `if (len2 > r2) continue`, `r2 = Range²` (`:2274-2276`); `Range` may be clamped by `--deferred_max_range` for **culling only** | hard: `if (d2 >= range*range) return false` (`:454`) | **AGREE** modulo the `>` / `>=` boundary and the never-set cull clamp |
| L3 | falloff | `1 - dist * rRange`, `rRange = 1/O->IRange` computed from the **real** range even when culling was clamped (`:5555-5558`, `:2287`) | `saturate(1 - d * invRange)` (`:460`) | **AGREE** — the CPU is unsaturated but `d ≤ range` makes it `[0,1]` anyway |
| L4 | `IRange` provenance | `O->IRange`, the animated spline value; greets defaults `greets_omni_default_range = 30` for FLD omnis authored at 0 | ingested from the same `Omni` | **AGREE** |
| L5 | `ISize` provenance | `O->ISize`; greets multiplies the three mech omnis' size spline by 1.5 (`GREETS.CPP:207-236`) | replicated through `Spline_Scale` (`SceneIngest.cpp:831-843`) | **AGREE** (this is the fixed §6.2d bug 3) |
| L6 | light→view transform | once per frame, `MatrixXVector(View->Mat, …)` (`:5535-5536`) | per pixel per light, `rowmul(camRow…, Li.pos - camSrc)` (`:449-450`) | **AGREE** — same rotation, different scheduling |
| L7 | spot cone | `cosTheta = -(dir·w)·lenInv`; hard reject at `cosOuter`; smoothstep `t²(3-2t)` up to `cosInner` (`:2291-2298`) | same shape, in world space (`:465-473`) | **AGREE** for the *diffuse* term. See D4 for specular |

### 2.3 Diffuse

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| D-a | direct diffuse | `k = NoL · falloff · cone`; `intensity = k · Mat->Diffuse · shadowAtten`; `lB += intensity·colB` (`:2287-2397`) | `diff = baseColor·diffuseK·(1-metal)`; `radiance += diff · L.color · (NoL·atten·shadow)` (`:537-540`) | **DIVERGE** on `(1-metal)` only — see D1 |
| D-b | **Lambert 1/π** | **absent, deliberately.** The only `1/π` in the CPU's direct term is inside the GGX `D` (`:2431`) | **absent, deliberately** (`:522-530`) | **AGREE** |

**Why the missing 1/π is correct and must not be "fixed":** the reference for this comparison is
the CPU image, and the CPU's Lambert has never been normalised — it is a long-standing engine
convention inherited from the forward `Lighting()` path, where `intensity = N·L · atten ·
Material::Diffuse` fed an 8-bit colour directly. Normalising *either* side alone puts the two
renderers π apart on every lit pixel. Normalising *both* would change the shipping look of every
scene, since all authored `Material::Diffuse` / `Omni::ISize` values were tuned against the
un-normalised term. GpuBench once carried the `1/π` and it was removed on measurement: with it, all
11 disco lights together moved the t=2000 frame by **at most 2/255 with 0 px changing by more than
2/255**; without it, 525 px change by >2/255 (MEASURED, §6.2b). The shader says so at the site.

### 2.4 Specular

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| S1 | model | Cook-Torrance, `sPbr` true (`:2424-2438`) | same (`:506-520`) | **AGREE** |
| S2 | lobe roughness | `rough = sqrt(2/(gloss+2))`, clamp `[0.04,1]`, `gloss = Glossiness ?: 32` (`:1790-1809`) | host-side, identical mapping (`Deferred.mm:880-886`); shader re-clamps (`:391`) | **AGREE** (this is the fixed §6.2d roughness bug) |
| S3 | GGX **D** | `a2/π / (d1² + 1e-6)`, `a = rough²`, `a2 = a²` (`:2430-2431`) | `a2 / max(π·d², 1e-7)` (`:286-290`) | **AGREE** — the guard differs (added vs `max`), immaterial |
| S4 | Smith **G** | `k = a/2`; `Gv = NoV/(NoV(1-k)+k)`, `Gl = NoL/(NoL(1-k)+k)` (`:1812`, `:2432-2434`) | `V_SmithSchlick = 0.25/((NoV(1-k)+k)(NoL(1-k)+k))`, i.e. `G/(4·NoL·NoV)` (`:292-297`) | **AGREE** — algebraically identical once the GPU's `· NoL` at `:540` is folded in (I checked the algebra term by term) |
| S5 | Fresnel | fixed dielectric `F = 0.04 + 0.96(1-VoH)^5` (`:2435-2437`) | identical (`:514-516`) | **AGREE** |
| S6 | `Material::Specular` | scales the whole lobe: `specStrength = spec · Mat->Specular · …` (`:2448`) | `S.specK = par.y · specularFactor` (`:386`) | **AGREE** numerically; see P1 for quantization |
| S7 | `NoV` clamp | `pbrNdotV = max(N·V, 1e-3)` (`:1813-1814`) | `saturate(dot(N,V))` then `max(·,1e-3)` (`:375`, `:519`) | **AGREE** |
| S8 | spec falloff | `(1 - dist·rRange)` (`:2449`) | `atten` (`:540`) | **AGREE** |
| S9 | spec × shadow | `· shadowAtten` (`:2449`) | `· shadow` (`:540`) | **AGREE** |
| S10 | **spec × spot cone** | **absent in the scalar path** — the cone smoothstep multiplies `k` (diffuse) at `:2296` and never reaches `specStrength` at `:2448` | `atten` carries the cone into `(diff + spec1·specTint)` (`:471`, `:540`) | **DIVERGE** — see D4 |
| S11 | `Specular_Factor` | a **gate**: `specGlobalOn = Specular_Factor > 0` (`:1361`, `:1788`) | a **multiplier**: `specK = par.y * u.specularFactor` (`:386`, `Deferred.mm:672`) | **AGREE** at greets' value 1.0; the semantics differ and would diverge at any other value |
| S12 | roughness map | attenuates **magnitude**: `specMul = 1 - roughness_strength · texel`, floored at 0, applied to `sB` (`:2531-2539`) | folded into `par.y` in the G-buffer: `spec *= max(0, 1 - roughTex.r)` (`:225-227`) | **DIVERGE (dial only)** — the GPU hard-codes strength 1.0; the CPU reads `--roughness_strength` (default 1.0, `FeatureFlags.def:169`). Identical today, silently divergent under any override |
| S13 | roughness map → **lobe** | **never widens the lobe** in the direct term… | …same | **AGREE** — but see E3: on the CPU the map *does* drive the **env** lobe |
| S14 | `Material::SpecMul` | multiplies the final specular incl. env (`:2578-2580`), default 1.0 (`Material.h:254`) | not ingested | **UNKNOWN** — inert if every greets material is 1.0; RVSF flag 0x800 can author it |

### 2.5 The `(1-F)` diffuse weighting — the recorded asymmetry

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| F1 | `--diffuse_energy` | on for greets (`GREETS.CPP:1113`). `fresEC` comes back from `EnvSpecComposeScalar` (`:1306`) and scales **`fdB/fdG/fdR`** (`:2568-2571`) — the **LDR** combine only. The `--hdr_linear` write uses `lB` (`:2625`) | no `(1-F)` at all (`:522-530`) | **AGREE** for the HDR frame greets actually ships |
| F2 | `(1-F)` under `--hdr` **without** `--hdr_linear` | `hB = fdB + sB` (`:2587`) — so `(1-F)` **does** apply on that path | absent | **DIVERGE**, but only in a configuration greets does not run. Recorded because a future `--no-hdr_linear` comparison would silently disagree |

### 2.6 Metalness

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| M1 | metal fetch | `MetallicMap` byte at `swizzledUV`, `/255` (`:2516-2519`) | `metalTex.sample(…).r`, then quantized to 8 bits through `gMirror.y` (`:232`, `:239`) | **AGREE** modulo requantization |
| M2 | **diffuse kill** | `fdB *= (1-metalM)` (`:2521-2523`) — **`fdB`, so LDR only**. `lB` is untouched, so the HDR frame keeps **full diffuse on conductors** | `(1 - S.metal)` on **both** direct diffuse (`:537`) and ambient (`:573`) | **DIVERGE — the largest one found. See D1** |
| M3 | spec tint | `sB *= 1 - m + m·texB/255` — the **gamma** texel (`:2541-2546`) | `specTint = mix(1, S.baseColor, metal)` — the **squared/linear** albedo (`:538`) | **DIVERGE — see D2** |
| M4 | metal → env | `f0 = max(Reflection/100, 0.04)` then `f0 + (0.98-f0)·metalM`; reflection tinted by albedo (`:1253-1255`, `:1307-1314`) | no env pano in this arm | **SCOPE** (plan §6.2e item 1) |

### 2.7 Normal map, tangent frame, handedness

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| N1 | G-buffer normal | oct **u32**, view space, `oct_decode_u32` (`:1635`) | oct in `RG16Snorm`, view space (`:236`, `:373`) | **AGREE** in meaning; the GPU is coarser (u16 vs u32 oct) |
| N2 | texel decode | 32-bit BGRA → R,G,B ×2−1; **16-bit RG** reconstructs `Z = √(1−x²−y²)` (`:171-188`) | `normalTex.sample(…).xyz * 2 - 1` (`:215`) | **AGREE** if the sidecars are 3-channel; **DIVERGE** for any `MakeNormal16` 2-channel map, which the GPU would read as `B=0 → nmZ=-1`. Which greets normal maps are BPP 16 is **UNKNOWN** to me from source alone |
| N3 | tangent | G-buffer oct-u16, re-orthogonalised `T -= N(T·N)`, renormalised (`:1682-1698`) | per-vertex engine tangent interpolated, same re-orthogonalisation (`:204-213`) | **AGREE** in meaning; the CPU pays an oct-u16 quantization the GPU does not |
| N4 | degenerate fallback | Mikkelsen: `ref = |ny|<0.9 ? (0,1,0) : (1,0,0)`, `T = normalize(cross(ref,N))` (`:1699-1710`) | identical (`:210-212`) | **AGREE** |
| N5 | **handedness** | `B = Mat->TbnHandedness · (N × T)` — mirrored-UV faces are split onto a `::mirUV` material clone carrying −1 (`:1711-1719`) | `bt = cross(n,t) · in.tangent.w` — the same sign, carried **on the vertex** because a de-indexed buffer needs no clone (`:214`) | **AGREE** in meaning. The plan (§3.1) already flags that the GPU therefore does **not** inherit the material split, so a defect localised to that boundary may simply not appear on the GPU |
| N6 | perturb | `N' = normalize(T·nmX + B·nmY + N·nmZ)` (`:1720-1727`) | identical (`:216`) | **AGREE** |
| N7 | **LOD fade** | `nmX,nmY *= max(0, 1 − (mip − start + 1)·step)`, `start=2`, `step=0.33` (`:1665-1670`, `FeatureFlags.def:126-127`) — bump is **fully flat from mip 5** | none | **DIVERGE — see D3** |
| N8 | map addressing | normal/rough/metal/AO all indexed by the **same** `miplevel` + `swizzledUV` as the albedo, i.e. **point-sampled at the parallax-shifted UV** (`:1659`, `:2517`, `:2532`, `:1899`) | each map sampled independently with hardware filtering at the **raw** UV | **DIVERGE — see D5 (parallax)** and, for filtering, **SCOPE** per A1 |

### 2.8 AO

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| O1 | source priority | `Mat_AoInAlpha` (albedo alpha) → `Mat->AoMap` → `ao_from_diffuse` (dev) (`:1856-1913`) | `mapFlags.z` (alpha) → `misc.y` (`aoTex`) → open (`:189-192`) | **AGREE** |
| O2 | AoMap width | 8-bit maps indexed as **bytes**; 32-bit fallback uses Rec.601 luma (`:1904-1911`) | `.r` of whatever was uploaded | **AGREE** in intent |
| O3 | strength | `ao = 1 − ao_map_strength · Mat->AoStrength · (1 − aoRaw)`, **not clamped** (`:1917`) | `ao = saturate(1 − 2.0·AoStrength·(1−aoRaw))` (`:192`, `Deferred.mm:898`) | **DIVERGE — see D6.** Also: the GPU hard-codes 2.0 rather than reading `--ao_map_strength` |
| O4 | what it multiplies | `lB` **before** the light loop (`:1918`) — and `lB` at that point is `Luminosity·255 + Diffuse·E(n)`, so **AO scales the emissive too** | `AmbientRadiance` only; the emissive add at `:608` is un-occluded | **DIVERGE — see D6b** |
| O5 | `--ao_direct` | moves the multiply to the final combined diffuse (`:2465-2468`); default OFF | not implemented | **SCOPE** (inert by default) |

### 2.9 Ambient / SH irradiance

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| B1 | model | L2 SH irradiance along the world shading normal, `lB = Luminosity·255 + Mat->Diffuse·E(n)` (`:1741-1760`) | `baseColor · diffuseK · (1−metal) · irr · ao` (`:573`) | structurally comparable; see B2–B5 |
| B2 | SH **source** | a real 32²×6 env-probe bake at the scene AABB centre, through the full deferred pipeline (`EnvBake.cpp:1688-1765`) | the FLD's authored zenith/nadir **backdrop gradient**, analytically projected (`Deferred.mm:238-272`) | **SCOPE/DIVERGE** — already the named residual (§6.2d); MEASURED as the ceiling's +7.28 signed dY (§6.2e) |
| B3 | **normalisation** | the bake folds `A_l/π` into the coefficients so `E(n)` is a direct env-colour-scaled irradiance — **a uniform env evaluates back to its own colour** (`EnvBake.cpp:1752-1758`) | `SH_Irradiance` uses the raw Ramamoorthi `c1..c5` constants (`:309-318`), which return **irradiance without the `1/π`** — a uniform env of colour `C` evaluates to `π·C` | **DIVERGE — see D7** |
| B4 | `Ambient_Factor` | greets sets `Ambient_Factor = 0.25` (`GREETS.CPP:3095`) but **FDS never reads it** — the only references in FDS are the definition (`RENDER.CPP:212`) and the extern (`FDS_VARS.H:228`). The deferred kernel's ambient carries **no such factor** | `fu.ambientFactor = 0.25f` multiplies `irr` (`Deferred.mm:670`, `:571`) | **DIVERGE — see D7** |
| B5 | emissive | `Mat->Luminosity · 255` inside `lB`, so the composite yields `albedo²·Lum·255` (0–255 units) | `S.baseColor · S.lum`, `lum = saturate(Lum·0.25)·4` (`:393`, `:608`) | **AGREE** in magnitude (`/255` unit conversion checks out) and **clamps at Lum = 4**; greets' hottest is 2.25 (plan §2.3), so live. AO application differs — D6b |
| B6 | flat `Sc->Ambient` fallback | `:1761-1768`, unreachable while `--sh_ambient` is on | n/a | **SCOPE** |

### 2.10 Environment reflection

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
> **§2.10 is superseded in part.** The "not implemented in this arm" verdicts below were
> written before §6.2f: GpuBench now **bakes its own six-face probes on the GPU** (its own
> rasterizer, so the CPU renderer stays out of the cost being measured) and runs
> `EnvSpecComposeScalar` term for term, including the split-sum and multiscatter helpers
> the shader had deleted. E1/E2/E4 are therefore **AGREE**, not SCOPE. The one that
> matters is the new **E0** row, and it is the largest CPU-side error found so far.

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| **E0** | **probe CONTENT encoding** | the face store is the bake render's 8-bit **VPage**, i.e. the LDR combine `texel*light/256 + spec` — **gamma** albedo at power 1 (`EnvBake.cpp` `renderSixFaces`, `DeferredSurfaceKernel.cpp:2509`) — and `EnvSpecComposeScalar` adds it **straight into the linear `sB`** the `--hdr_linear` frame is built from (`:2649`) | the probe faces are the lighting pass's own **RGBA16Float linear radiance**, added into the same linear accumulator | **DIVERGE — the CPU is wrong, and it is the whole conductor gap.** MEASURED (§6.2k): same probe, same 256², same units, CPU faces mean luma 42.53 vs 23.32 — **1.82x**, against a 1.66x disagreement on the lit conductor mask. Mechanism: a reflection is brighter than the thing it reflects by ≈`255/albedo`. Same failure class as D1 and F1 — a term written for the pre-`hdr_linear` gamma composite and never converted. Fix implemented behind `--env_bake_linear`, **default OFF**: probe content then agrees to 2.8 % and the conductor ratio falls 1.657 → 1.121 |
| E1 | when it applies | `envP = (env_refl && (Mat->Reflection > 0 || metallicMap)) ? envTab[matID] : nullptr` (`:1864-1867`) — **per material**, and only if a pano was baked for it | same qualification, replicated in the ingest (`SceneIngest.cpp:1474-1503`), 6 probes on greets | **AGREE** since §6.2f. (The earlier "SCOPE" was correct at the time and is the fixed §6.2d bug 2 — applying the helpers *unconditionally* cost **+24 mean luma**, MEASURED) |
| E2 | split-sum | Karis "Mobile" polynomial `envBrdf = f0·A + B`; `ek = envBrdf · env_refl_gain` (`:1257-1274`) | identical polynomial (`deferred.metal:718-728`) | **AGREE** |
| E2b | env **F0** | `f0 = max(Reflection/100, 0.04)`, then `f0 + (0.98-f0)·metalM` (`:1253-1255`) | identical, folded in the G-buffer and quantized to 8 bits through `gMirror.w` (`deferred.metal:291-294`) | **AGREE** modulo 1/255 requantization |
| E3 | env lobe roughness | the **roughness map texel** if present, else `sqrt(2/(gloss+2))` (`:1096-1102`) — so on the CPU the map *does* drive the env lobe even though it only scales magnitude in the direct term | always the gloss-derived `S.rough`; the roughness map is folded into `par.y` (magnitude) and never reaches the env mip select (`deferred.metal:714-716`) | **DIVERGE (unpriced).** A candidate for the 1.12x residual §6.2k leaves open. Also worth recording: S13's "never widens the lobe" is true of the *direct* term only |
| E3b | **store geometry** | six **1.25-padded** faces at 102.68°, face-major **bilinear** in software, 4-level box-downsample mip chain | plain 90° hardware `texturecube`, hardware trilinear, `generateMipmaps` | **DIVERGE (unpriced)** — the other candidate for the 1.12x residual |
| E4 | multiscatter | Fdez-Aguera `1 + Fms(1−Ess)/Ess`, scaling **`ek` only**, leaving the `(1−F)` handed to `--diffuse_energy` at single-scatter (`:1288-1293`) | identical (`deferred.metal:730-734`) | **AGREE** since §6.2f |
| E5 | parallax / cv-pull / SSR / live-water | `:1001-1244` | scene-AABB slab-exit parallax only (`deferred.metal:698-708`); no cv-pull / SSR / live-water | **SCOPE** for the city-only features; the AABB parallax proxy **AGREE**s |

### 2.11 Shadows

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| H1 | chain | `computeMapShadowAtten` (mirror-clone source map + source cube + own 2-D spot map, PCF 2×2 bilinear) × `resolveCubeAtten` (static lightmap **or** cube tap, PolyId identity compare) × `--pom_horizon` (`:2313-2390`) | one hardware `sample_compare` per light (`:404-437`) | **SCOPE by design** — the PolyId test and the static-shadow lightmap are amortisations of an expensive CPU tap (plan §3); the GPU takes every tap the CPU may skip |
| H2 | bias | `shadow_bias` + `shadow_slope_bias` integers, applied in the CPU's own depth encoding | `bias = mix(0.0025, 0.0004, NoL)` in reversed-Z (`:413`) | **UNKNOWN** — not comparable without a per-tap dump; only aggregate agreement has ever been measured (§6.2d: shadowed direct +21.46 CPU vs +18.70 GPU) |
| H3 | where it multiplies | diffuse and specular both (`:2391`, `:2449`) | `(diff + spec1·specTint) · … · shadow` (`:540`) | **AGREE** |

### 2.12 Tonemap, exposure, bloom, post

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| T1 | exposure | `kE = hdr_exposure/255`; greets' `cine::kGreetsExposure` is **1.0** (`Hdr.cpp:813-819`) | `c * u.exposure`, default 1.0 | **AGREE** |
| T2 | ACES | Narkowicz `(x(2.51x+0.03))/(x(2.43x+0.59)+0.14)`, saturated (`Hdr.cpp:833-836`) | identical (`:859-862`) | **AGREE** |
| T3 | `hdr_white` chroma pull | applied on the ACES-linear values with Rec.709 weights (`Hdr.cpp:843-846`) | not implemented | **AGREE at default 1.0**; **DIVERGE** under any override |
| T4 | linear encode | `sqrt` when `hdr_linear` (`Hdr.cpp:847-851`) | `c = sqrt(c)` unconditionally (`:871`) | **AGREE** for greets; the GPU has no gamma-path branch |
| T5 | bloom | soft-knee bright pass, DS=4 box, `[1 4 6 4 1]/16` twice, bilinear upsample × intensity, added **before** tonemap; threshold 200 on the 0–255 scale | same construction, threshold `/255` (`:1141-1222`) | **AGREE**; not reproduced: `HdrClamp` on add-back and the shared bright-pass cache |
| T6 | LDR post (CA, vignette, grade, grain) | `Hdr.cpp:701-800` | not implemented | **SCOPE** (all default OFF outside `--cinematic`) |

### 2.13 Rate, coverage, precision

| # | quantity | CPU | GPU | verdict |
|---|---|---|---|---|
| P1 | material param precision | `float` throughout | `Diffuse, Specular(×roughmap), rough, Luminosity/4` packed into **`RGBA8Unorm`** (`Deferred.mm:314`, `:502`, `:872-887`) | **DIVERGE (precision)** — 1/255 quantization and a hard clamp at 1.0 (4.0 for `Luminosity`). At `MARB4`'s `Specular = 0.05` that is ~2% on the whole spec lobe; INFERRED, not measured. Any `Diffuse > 1` would clamp silently |
| P2 | shading rate | `deferred_checkerboard` **ON** for greets (`GREETS.CPP:1183`) — half the pixels are reconstructed by the fill wave (`:4793`), not shaded | full rate | **SCOPE** for cost (plan §5.3 item 11); for *fidelity* it means half the CPU frame is an interpolation of neighbours, which any per-pixel comparison must account for |
| P3 | approximate math | `fast_rsqrt` (NR-corrected), `_mm256_rcp_ps` in the vec paths, `fastPow2/fastLog2` in the non-templated gloss fallback | full-precision `rsqrt`/`sqrt` | **AGREE** within ~1e-6 for the scalar path greets runs |
| P4 | depth | `ZPage16`, 16-bit, `0xFF80 − round(g_zscale·z)` | `Depth32Float`, reversed-Z | **SCOPE** (plan §5) — the GPU is strictly more precise |

---

## 3. Divergences found — the primary deliverable

Ranked by how much I expect them to move a pixel. **None of these were fixed.** Everything below
is read out of source; where I write a number and did not run it, it says INFERRED.

### D1 — the CPU's HDR frame does **not** kill diffuse on conductors; the GPU does

`--hdr_linear` builds the frame from `rlB = aB·aB·lB + sB` (`:2625`) — the **raw** accumulator
`lB`. The metalness diffuse-kill is applied to `fdB`, the LDR combine, at `:2521-2523`:

```cpp
float fdB = (texB * lB) * (1.0f / 256.0f);      // :2509
...
if (metalM > 0.0f) { const float dk = 1.0f - metalM; fdB *= dk; ... }   // :2521-2523
...
float rlB = aB*aB*lB + sB;                       // :2625   ← lB, not fdB
```

So on the shipping greets frame a conductor keeps its **full diffuse**. The GPU removes it twice —
`(1 - S.metal)` in `DirectRadiance` (`:537`) and again in `AmbientRadiance` (`:573`).

This is structurally identical to the `(1-F)` asymmetry that §6.2d found and recorded, but the
metalness half was never noticed: the GPU comment at `:531-536` states the CPU "kills diffuse on
metal pixels" and cites `:2512-2545`, which is true of the LDR path and false of the HDR path the
frame is actually built from.

**Numeric consequence (INFERRED):** at `metalness = 1` the GPU renders **zero** diffuse where the
CPU renders the full `albedo² · Diffuse · Σ(NoL·atten·shadow·colour) + albedo² · Diffuse · E(n)`.
The affected materials are exactly the RVSM metallic sets — `momy`, `momy2`, `amudim`, and the
`amudim` columns dominate the t=2000 pose. §6.2e measured those columns at GPU `(84,68,48)` vs
reference `(96,77,53)` and attributed the deficit entirely to the missing env panorama. **At least
part of that gap is this.** It is a strictly darkening error on the GPU, in the same direction as
the env gap, which is why the two were not separable by eye.

**This is the single most valuable row in the document**, because a whole-frame diff at the pose
where it is largest reads as "metal is darker, we know why" — the correct-looking story hid a
second cause.

### D2 — the metal specular tint uses gamma albedo on the CPU, linear albedo on the GPU

CPU (`:2541-2546`): `sB *= 1 - m + m · texB · (1/255)` — `texB` is the **gamma** 0–255 texel.
GPU (`:538`): `specTint = mix(1, S.baseColor, metal)` — `S.baseColor` is the **squared** albedo.

For a mid-tone conductor texel of 0.5 gamma at `m = 1`, the CPU tints by 0.5 and the GPU by 0.25 —
the GPU's metal highlight is **2× too dark** (INFERRED). It vanishes at albedo 0 and 1 and peaks at
mid-tones, i.e. exactly the `amudim`/`momy` range. Same sign as D1, same pose, same materials: the
two compound.

### D3 — the CPU fades the normal map with mip level; the GPU never does

CPU (`:1665-1670`): `nmX, nmY *= max(0, 1 − (mip − 2 + 1)·0.33)`. At mip 2 the bump is at 67%, at
mip 4 it is at 1%, from mip 5 it is **exactly flat** and the shading normal is the geometric one.
The GPU has no equivalent — its mip chain averages the normal texels but the perturbation never
goes to zero.

This was near-inert while `--mips` was forced off (every face rasterised at level 0). **It stopped
being inert on 2026-08-08**: `b8319e1` flipped `mips` to default ON and MEASURED 7.6% of draws /
83.3% of *area* at level 0, with **48.8% of draws at level 6** (`FeatureFlags.def:278`). Every one
of those draws is a surface where the CPU shades flat and the GPU shades bumped. A third agent is
flipping this default right now, so the divergence is actively growing while this document is being
written.

Untestable by frame comparison at the primary review pose, because the near wall that pose looks at
is at level 0 on both arms.

### D4 — the CPU's scalar specular ignores the spot-cone penumbra

In the scalar path the cone smoothstep multiplies **`k`** only (`:2296`), and `k` feeds the diffuse
accumulator at `:2391`. The specular at `:2448` is `spec · Mat->Specular · (1 − dist·rRange) ·
shadowAtten` — no cone factor. Outside the cone the loop `continue`s (`:2293`) so specular is zero;
**inside the penumbra it is at full strength.**

The vec path does not have this gap — it explicitly stashes `coneAtten` into `coneShadowAtten`
(`:2090`) precisely so the templated spec loop can apply it (`:356`), and the comment there calls
the previous behaviour "the documented vec-spec gap". The fix landed in the vec path and not in the
scalar one; on arm64 the scalar path is the only one that runs.

The GPU applies the cone to both terms (`:471`, `:540`), so **the GPU is the one behaving
correctly** and the CPU has a specular ring in every spot penumbra. Affects the 10 disco spots
only — invisible at t=5743 (§6.2b MEASURED: every disco spot reaches 0 px there) and live at
t=2000.

### D5 — the GPU has no parallax/POM; the CPU runs an 8-step march on `rooms`

`BatchUniforms.mapFlags.w` is documented as `parallaxScale` (`:61`) and ingested
(`Deferred.mm:891`), but **no shader function reads it** — `grep parallax GpuBench/shaders/*.metal`
returns only that comment. Meanwhile `rooms` is the one greets material with both a non-zero
authored `ParallaxScale` (0.10) and a height map, and `--parallax` / `--parallax_pom` both default
ON (plan §2.3). On the CPU the march shifts the UV used for the albedo **and** for the normal,
roughness, metal and AO fetches (they all index `swizzledUV`); on the GPU every map is sampled at
the geometric UV.

I believe this is *known* in the sense that the plan treats displacement as a future GPU arm — but
it is not written down as a **shading-parity** item, and it means every per-texel comparison on the
stone wall is comparing different texels.

### D6 — AO: the CPU can drive the ambient negative; the GPU saturates

CPU (`:1917`): `ao = 1 − ao_map_strength · Mat->AoStrength · (1 − aoRaw)` with
`ao_map_strength` defaulting to **2.0** (`FeatureFlags.def:137`) and **no clamp**. For any AO texel
below 0.5 this is **negative**. `lB *= ao` then flips the sign of the ambient+emissive term, and
the clamp at `:2492-2494` only fires **after** the direct light has been added — so on the CPU a
dark AO texel actively **subtracts** direct light. The GPU `saturate`s at `:192`.

The greets wall/floor albedo sidecars carry baked AO in alpha (`Mat_AoInAlpha`,
`GREETS.CPP:1557-1567`), so any mortar texel below 128 takes this path. Magnitude is INFERRED, not
measured, but the mechanism is unambiguous and it is regional (grooves), which is precisely the
kind of thing a whole-frame mean hides.

### D6b — the CPU occludes the **emissive** term; the GPU does not

At the point of `lB *= ao` (`:1918`), `lB` already contains `Mat->Luminosity · 255` (`:1753`). So
CPU AO scales emission. The GPU adds emission after and outside `AmbientRadiance` (`:608`), so it
is never occluded. Only bites on materials carrying **both** a Luminosity and an AO source —
`screen emiter` is a candidate (it ships a full RVSM set). Magnitude UNKNOWN.

### D7 — the ambient is off by `π/4` in the GPU's favour of darkness

Two independent factors that partly cancel:

- **The GPU omits the `1/π` irradiance→radiance conversion.** The CPU's bake folds `A_l/π` into the
  coefficients so that a uniform environment of colour `C` evaluates back to `C` exactly
  (`EnvBake.cpp:1752-1758`, and the comment says so). The GPU's `SH_Irradiance` (`:309-318`) uses
  the raw Ramamoorthi constants, whose DC term is `c4 = 0.886227`; for a uniform env,
  `L₀₀ = C·Y₀·4π = 3.5449·C` and `E = 0.886227 · 3.5449 · C = π·C`. So the GPU's `irr` is **π×**
  the CPU's convention.
- **The GPU then applies `ambientFactor = 0.25`** (`Deferred.mm:670`), sourced from
  `GREETS.CPP:3095`'s `Ambient_Factor = 0.25`. But `Ambient_Factor` is **dead in FDS** — the only
  occurrences in the whole engine are the definition (`RENDER.CPP:212`) and the extern
  (`FDS_VARS.H:228`); the deferred kernel's SH ambient (`:1741-1760`) never reads it, and neither
  does anything else in `FDS/`. Only `DEMO/CITY.CPP:903` uses it, for a local forward-path `Ka`.

Net: the GPU's ambient carries `π × 0.25 = 0.7854` where the CPU carries `1.0` — i.e. the GPU is
**~21.5% dark** in ambient, per unit of source irradiance (INFERRED; the algebra above is the
derivation).

This is invisible to frame comparison because it is confounded with B2 — the two arms draw their SH
from entirely different sources (probe bake vs backdrop gradient), so a brightness disagreement was
attributable either way. The note in `Deferred.mm:713-715` that "the SH ambient projects the
UNSQUARED sky gradient and matched the CPU base within 1.4%" is a *whole-frame* agreement that these
two errors, plus the source difference, happened to produce jointly. **They should be separated.**

### D8 — untextured materials

`:1527` — `if (!Mat || !Mat->Txtr) continue;` — the CPU deferred kernel does not shade pixels whose
material has no texture. Three of greets' 26 material bindings are untextured (plan §2.3). The GPU
shades them from `baseColor` (`:178`). Small, but it is a coverage difference, not a shading one,
and coverage differences are the ones that produce confusing diffs.

### D9 — dial hard-codes on the GPU

Not divergences today; silent divergences the moment anyone passes the flag. All in
`Deferred.mm`/`deferred.metal`: `ao_map_strength` fixed at 2.0 (`:898`), `roughness_strength` fixed
at 1.0 (`deferred.metal:227`), `hdr_white` absent, `Specular_Factor` used as a multiplier where the
CPU uses it as a gate (S11). Each should either read the flag or assert the flag is at its default.

---

## 4. Quantities that are UNTESTABLE by frame comparison

These are the phase-2 target list. Each is either invisible at the review poses, or visible only in
confounded aggregate.

| quantity | why frame comparison cannot settle it |
|---|---|
| **D1** metal diffuse-kill | only conductors, and only at the disco pose where the missing env pano moves the same pixels the same direction. A signed diff cannot attribute |
| **D2** metal spec tint colour space | same pixels as D1, same sign, and it is a *tint*, so it also moves hue — the one channel the frame diff reports is luma |
| **D3** normal-map LOD fade | needs a pose where the wall is at mip ≥ 2. The review poses look at the near wall (mip 0). Also entangled with the CPU/GPU texture-filter gap (A1), which dominates the same high-frequency band |
| **D4** spot-cone specular | only inside a spot penumbra, only when a specular material is there. At t=5743 every disco spot reaches **0 px** (MEASURED, §6.2b) |
| **D6/D6b** AO clamp + emissive occlusion | sub-pixel-scale, regional, and folded into whatever the mortar looks like; the AO source is only in the alpha channel so no viz shows it directly |
| **D7** ambient `π` and `Ambient_Factor` | confounded with B2 (probe bake vs backdrop gradient). Whole-frame luma agrees to +0.19/255 (MEASURED §6.2e) with **three** errors in the ambient path |
| S12/S14/A3 dial + `SpecMul` + `Tint` | inert at defaults; a comparison run at defaults proves nothing about them |
| P1 8-bit param quantization | ≈2% on one term, well under the frame-diff noise floor and under the filtering speckle |
| H2 shadow bias | aggregate only. The two arms use different depth encodings and different filters; only a per-tap dump can compare them |
| N2 16-bit normal maps | a wrong `nmZ` on a 2-channel map would look like "slightly different bump", indistinguishable from N1/N3 quantization at a glance |
| **L1–L5 light inventory** | not a shading question at all. A wrong `ISize`, `IRange` or colour reproduces a *plausible* frame. The `OmniSizeMult` bug (§6.2d) survived a commit that deleted its sibling and was found only by isolating per-pixel ratios |
| UV generation / retile | likewise: the floor's 1.5×-too-dense retile is a *data* defect that renders as a perfectly plausible floor |

The last two rows are the important structural point: **two of the five known bugs were not shading-math bugs at all.** They were scene-data bugs. A numeric kernel harness cannot catch them, and any design that claims it can is wrong. See §5.3.

---

## 5. Phase 2 — the numeric harness

Constraint from the brief, and it is the right one: **no refactor of either kernel.** A harness that
re-implements the shading math to compare against it is the mode-10 trap the GPU arm already fell
into (`deferred.metal:320-336`) — a diagnostic that cannot fail its own test.

### 5.1 The shape: drive the REAL kernels with synthetic inputs

Neither side needs new shading code, because both expose an entry point that takes its inputs from
memory.

**CPU.** `Render_DeferredLighting(DeferredLightingCtx&, const DeferredOverride*)` is **public**
(`DeferredCommon.h:302`) and `DeferredOverride` (`:285-298`) already lets a caller supply its own
`gb`, `vpage`, `zpage16`, `tileLights`, `xres`, `yres`. So a new target under
`tools/shading_parity/` can, with **zero edits to `FDS/RENDER/`**:

1. build a synthetic `Scene` — one `Omni` per light row (so `colB = L.B·ISize` and `rRange = 1/IRange`
   are exercised by the **real** list builder at `:5532-5558`, not re-derived), and one `Material`
   per material row with in-memory 1×1 `Texture` mips for albedo / normal / roughness / metal / AO
   (so the real `decodeNormalTexel`, the 8-bit AO/rough/metal fetches and the `Mat_AoInAlpha` path
   all run);
2. allocate a `W×1` G-buffer and write, per row `i`: `txtr = mip<<28 | matID<<20 | swizzledUV`,
   `normal = oct_encode_u32(N)`, `tangent = oct_encode_u16(T)`, `mirrorId`, `shadowMatID`;
3. set `ZPage16[i]` so the kernel's own reconstruction (`z = (0xFF80−zEnc)·invZScale`,
   `x = (px−CntrEX)·z·invFOVX`, …) lands on the row's intended view-space point. **The table is
   therefore parameterised by `(px, py, zEnc)`, not by an arbitrary `P`** — the view ray is a
   constraint of the design, not a limitation worth fighting;
4. size `g_hdrBuf` for that target, run the pass, read back `g_hdrBuf[i*4 + 0..2]` — the **linear
   radiance**. Reading VPage instead would lose exactly the D1/F1 asymmetry, which lives only in
   the HDR write. This is a hard requirement of the design. The one wrinkle: `Hdr_BeginFrame`
   sizes from `MainRenderTargetFromGlobals()`, and the kernel gates its HDR write on
   `Hdr_WritableFor(ctx.xres, ctx.yres)` (`:1373`) — so the harness must point the engine's
   `XRes`/`YRes`/`VPage` globals at its own `W×1` target before calling it. All of
   `g_hdrBuf` / `Hdr_BeginFrame` / `Hdr_WritableFor` / `HdrClamp` are public in `FDS/RENDER/Hdr.h`.

**GPU.** The Metal shading code is a **runtime-compiled `.metal` file** (`shaderPath` is a runtime
argument). A separate host in `tools/shading_parity/` can compile *that same file* and dispatch
*those same fragment functions* — so the GPU half also needs **zero edits to `GpuBench/`**. Per row:
a degenerate full-viewport draw with the row's `BatchUniforms` and 1×1 textures through
`vs_gbuffer`/`fs_gbuffer`, then `fs_lighting` over the resulting one-pixel G-buffer, then read back
the RGBA16Float target. Running the real `fs_gbuffer` (rather than stamping the G-buffer from the
host) is what makes the GPU half cover TBN construction, the roughness→`par.y` fold, the AO
strength fold and the 8-bit param quantization — i.e. the same span the single CPU call covers.

The one duplication hazard is the C-side mirror of `FrameUniforms`/`BatchUniforms`/`GpuLight`.
Mitigate with `static_assert(sizeof(...))` against the values `Deferred.mm` already uses, and put
the mirror in one header the tool owns.

### 5.2 The table, the columns, and what "pass" means

**Rows (~256, checked in as TSV).** A full-factorial sweep is wrong — it is dominated by
uninteresting combinations. The right shape is *one axis varied per block*, each block sized to make
one contract row fail loudly:

| block | rows | varies | contract rows it pins |
|---|--:|---|---|
| light colour | 12 | `L.R/G/B` ∈ {255,128,64,0} × `ISize` ∈ {0.5,1,1.5} | L1, L5 |
| range/falloff | 16 | `d/IRange` ∈ [0,1.05] | L2, L3 |
| diffuse | 8 | `Mat->Diffuse`, `NoL` | D-a, D-b, P1 |
| gloss sweep | 28 | `Glossiness` ∈ {4,8,16,32,48,64,128} × 4 off-peak `(N,V,L)` | S2, S3, S4, S5, P1 |
| Fresnel | 8 | `VoH` ∈ [0,1] at fixed roughness | S5 |
| roughness map | 8 | rough texel ∈ [0,1] | S12, S13 |
| **metalness** | 12 | metal ∈ {0,0.5,1} × albedo ∈ {0.2,0.5,0.8} × (spec, diffuse) | **D1, D2, M1** |
| **AO** | 12 | AO texel ∈ {0,0.25,0.5,0.75,1} × `AoStrength` × `Luminosity` ∈ {0,1} | **D6, D6b, O3** |
| normal map | 16 | texel, handedness ±1, BPP 16 vs 32, mip 0..6 | N2, N5, **D3** |
| ambient | 12 | 6 normal directions × 2 SH sets | B1, **D7**, B5 |
| spot | 12 | `cosTheta` across the penumbra, spec on/off | L7, **D4** |
| emissive | 6 | `Luminosity` ∈ {0,0.5,1,2.25,4,5} | B5 (incl. the clamp) |

**Columns (this is the part that matters).** Each row must report the terms **separately**, not
just a total: `ambient`, `emissive`, `direct_diffuse`, `direct_spec`, `env` — each as linear B,G,R.
A total-only comparator lets a 2× error in one term hide under another; every one of the five known
bugs lived in exactly one term.

Getting the split out of the CPU without touching the kernel is done by **ablation**, using flags
that already exist: `--prof_no_lights` (`:1396`) isolates ambient+emissive; `--prof_no_spec`
(`:1397`) removes the spec term; `Luminosity = 0` rows isolate ambient from emissive; a
zero-`Diffuse` row isolates emissive. That is 3–4 runs of a millisecond-scale harness — free — and
it means the split is derived from the real kernel's own gates rather than from a parallel
implementation. Same trick on the GPU with `--viz=ambient|direct` and `lightRangeScale=0`, which
`Deferred.mm` already supports.

**Tolerance, and where it comes from.** Per term, per channel:

```
pass  ⇔  |gpu − cpu| ≤ 0.005 · max(|cpu|, |gpu|)  +  1e-4
```

- the `1e-4` absolute floor is the RGBA16Float mantissa near zero plus the CPU's `HdrClamp`;
- the 0.5% relative band is dominated by **P1**, the GPU's 8-bit `params` quantization: `Specular`
  at 1/255 is 0.2% at `Specular = 0.4` and 2% at `Specular = 0.05`, so **rows whose material has
  `Specular < 0.1` get a per-row widened band of 1/(255·Specular)**, stated in the table rather than
  applied globally — a blanket 2% band would swallow D2's 2× error class at low albedo;
- normal quantization (u16 oct on the GPU vs u32 on the CPU, ~0.1° angular) is amplified by the GGX
  lobe; at `rough = 0.2` a 0.1° normal error is ≲1% of the spec peak, which the 0.5% band would
  fail. **So the normal-map and gloss blocks specify their normals as exactly-representable oct
  values on both sides** — the table is authored in oct-quantized form, which removes the term
  rather than budgeting for it.

A row "passes" when **every term × every channel** passes. The comparator prints, for each failure,
the term, the channel, the ratio, and the contract row id from §2 — so a failure names the
divergence instead of reporting a number.

### 5.3 Validating the design against the five known bugs

The brief's test: *a harness that would not have caught the squared light colour in seconds is not
the right design.* Checked honestly:

| known bug | caught? | by what |
|---|---|---|
| **squared light colour** | ✅ seconds | the light-colour block. A row with `(0,128,255)` gives `direct_diffuse` channel ratios 1 : 0.502 — a 99% failure on G and B with R exact, which is a *signature*, not just a failure |
| **`FssEss/Fms` env term on every pixel** | ✅ seconds | any row with `Reflection = 0` and no metallic map. The `ambient` column would differ while `direct_*` matched |
| **GGX lobe rough 0.625 vs 0.2** | ✅ seconds | the gloss sweep. At `Glossiness = 48` off-peak the `direct_spec` column differs by >2× |
| **`OmniSizeMult` 1.5 surviving** | ❌ **NO** | the table *supplies* `ISize`, so both arms use the same value. This is a scene-**data** divergence |
| **floor UV retile 1.5× too dense** | ❌ **NO** | same — a UV-generation divergence, invisible to a kernel fed explicit inputs |

**3 of 5.** That is the honest score, and the two misses are the same class. So the harness must be
**two parts**, and the second is cheaper than the first:

**Part B — the data census diff.** Both arms already have most of it. Dump, from each side, a
canonical text census at a fixed `CurFrame` and diff it offline:

- **lights**: index, world position, `L.RGB`, `ISize`, `IRange`, `Type`, `HotSpot`, `FallOff`,
  `IDir`, casts-shadow, shadow resolution. GpuBench already prints `[DEFERRED] LIGHT INVENTORY`
  (`Deferred.mm:739-774`) — it needs a machine-readable sibling, and the CPU needs the same. **This
  catches `OmniSizeMult` in seconds**, and it would have caught it the day the commit that deleted
  the Range tables left the Size table behind.
- **materials**: name, `Diffuse`, `Specular`, `Glossiness`, `Luminosity`, `Reflection`,
  `AoStrength`, `SpecMul`, `TintRGB`, `TbnHandedness`, `ParallaxScale`, flags, and the resolved
  file path of each of the six map roles. **This catches A3, S14, and any future sidecar repoint.**
- **geometry**: per material — face count, UV bounding box, total UV area / total world area (the
  texel density), and a hash of the tangent+handedness stream. **This catches the floor retile**,
  because a 1.5× retile is a 2.25× texel-density ratio in one number.

Part B needs no GPU, no Metal, and no kernel. It is a few hundred lines and it covers the half of
the failure space the numeric harness structurally cannot.

**Combined: 5 of 5.**

### 5.4 What the harness still would not catch

Stated so nobody over-trusts it: shadow tap agreement (H2 — needs a per-tap dump against a
ray-cast ground truth, which `--probe` already prototypes on the GPU side), the checkerboard fill
wave (P2 — a *reconstruction*, not a shading expression), the mip-selection difference (A1), and
anything about the rasterizer's coverage. Those stay frame-comparison territory.

---

## 6. Hooks needed inside contended files — REPORTED, NOT EDITED

Three other agents currently own `FDS/FILLERS/`, `FDS/RENDER/`, `DEMO/MeshOps.cpp`,
`DEMO/GREETS.CPP`, `FDS/FRUSTRUM/FRUSTRUM.CPP`, `FDS/IMGCODE/`, `FDS/Base/RenderStats.*` and
`GpuBench/**`. Phase 1 touched none of them. For phase 2:

**Good news: the core harness needs no hook.** `Render_DeferredLighting` + `DeferredOverride` are
public, and the Metal shaders are runtime-compiled from a path. Both halves can live entirely under
`tools/shading_parity/`.

Two optional hooks would make it better, and both are for **Part B**, not Part A:

| # | file | what | why |
|---|---|---|---|
| K1 | `FDS/RENDER/DeferredSurfaceKernel.cpp` (or better, a new `FDS/RENDER/SceneCensus.cpp`) | a `void Scene_DumpCensus(Scene*, FILE*)` emitting the light + material census of §5.3 Part B in a stable text format | the CPU has no equivalent of GpuBench's `LIGHT INVENTORY`. **Preferred placement is a NEW file** so no contended file is touched at all — it only needs `Scene`, `Omni`, `Material`, all public |
| K2 | `GpuBench/Deferred.mm:739-774` | make the existing `LIGHT INVENTORY` printf emit the same stable format under a flag (`--census`) | so the two censuses diff directly instead of being reformatted by a parser that can drift |

**K1 as a new file needs no permission from the contended set.** K2 does — it is one printf format
in `GpuBench/`, owned by another agent right now. **Reporting it rather than editing.**

One further note for whoever picks up phase 2: if the harness ever *does* want a per-term split
without the ablation trick of §5.2, the correct change is the one the GPU arm already made for its
viz modes — hoist the per-pixel body of `Render_DeferredLighting_Tile` into a `static inline
shadeDeferredPixel(const ShadeInputs&, ShadeOutputs&)` that the tile loop and a dump entry point
both call, so the two can never compute different quantities. That is a real refactor of the most
contended file in the tree and it is **not** required by the design above. It is recorded as the
fallback, not the plan.

---

## 7. Method note

Everything in §2 and §3 was read out of the two sources; nothing was rendered for this document.
That is deliberate — the point of the exercise is that a contract can be checked without running a
frame, and every row a frame-diff *would* have caught was already caught. The rows that matter
(D1, D2, D3, D4, D6, D6b, D7) are the ones a frame diff could not have separated, and each of them
is stated with the mechanism and the citation so it can be argued with rather than believed.

Where a consequence is arithmetic I derived (D2's 2×, D7's π/4), it says INFERRED. Where a number
came from a run someone did, it says MEASURED and points at the section of
`GPU_BENCHMARK_PLAN.md` that owns it. Where I could not establish something from source — A3, N2,
S14, H2, D6b's magnitude — it says UNKNOWN, and UNKNOWN is not a soft AGREE.
