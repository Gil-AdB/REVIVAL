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
| E3 | env lobe roughness | the **roughness map texel** if present, else `sqrt(2/(gloss+2))` (`:1096-1102`) — so on the CPU the map *does* drive the env lobe even though it only scales magnitude in the direct term | always the gloss-derived `S.rough`; the roughness map is folded into `par.y` (magnitude) and never reaches the env mip select (`deferred.metal:714-716`) | **DIVERGE, and INERT ON THE MECH** — no mech material has a roughness map at all (§11.1), so E3 cannot be the mech's discrepancy. Still a candidate for the 1.12x residual §6.2k leaves open. Also worth recording: S13's "never widens the lobe" is true of the *direct* term only |
| E3b | **store geometry** | six **1.25-padded** faces at 102.68°, face-major **bilinear** in software, 4-level box-downsample mip chain | plain 90° hardware `texturecube`, hardware trilinear, `generateMipmaps` | **DIVERGE — the mip-chain DEPTH half is now priced, see E7 and §11.** The padding/face-geometry half remains unpriced |
| E4 | multiscatter | Fdez-Aguera `1 + Fms(1−Ess)/Ess`, scaling **`ek` only**, leaving the `(1−F)` handed to `--diffuse_energy` at single-scatter (`:1288-1293`) | identical (`deferred.metal:730-734`) | **AGREE** since §6.2f |
| E6 | animated meshes in the probe | **EXCLUDED** — `EnvBake.cpp:311` sets `g_envBakeSkipDynamic`, `Transform.cpp:1274` folds it into `inStaticBake`, `:1559` skips the mesh. On greets that is the **whole mech** — 6 distinct meshes in the `[STATIC-BAKE-SKIP-MESH]` log | **INCLUDED** — no such rule; only the probe's own material is skipped | **DIVERGE — priced in §11.** `--env_bake_skip_animated` (GpuBench, default OFF): 5,268 px, mean \|Δ\| 24.94 |
| E7 | env mip-chain DEPTH | `EnvPanoLinear::kMaxMips = 4` (`EnvBake.h:63`), fixed, whatever the face res | `mipmapped:YES` + `generateMipmaps` → the FULL chain (8 levels at 128²) | **DIVERGE — priced in §11.** The select formula is identical but the divisor is not: `cockpit`'s `rough = 0.174` lands at ≈178² effective on the CPU and ≈55² on the GPU, a **3× wider lobe**. This is the dominant half of E3b |
| E8 | Sobel-from-diffuse normal map | `GREETS.CPP:1967` bakes one for every material named `*hull*` / `*cockpit*` (+ stairs/amudim/floor/marb, rooms/siling) that lacks one — on greets: `cockpit`, `hull not smooth`, `hull`, `siling` | none — `SceneIngest.cpp:1409` takes only RVSM/sidecar normals | **DIVERGE — priced in §11.** `--nmap_strength=0`: 311,080 px; 3.3 % of hull pixels > 10 luma, max 162.7. This is the **specular-highlight** difference the user reported |
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

---

## 8. The `--hdr_linear` MIGRATION AUDIT — every term that feeds the HDR accumulator

**Why this section exists.** Three defects found separately turned out to be one defect: a term
written for the pre-`hdr_linear` **gamma** composite that was never converted when the composite
went **linear**. The conductor diffuse kill (D1 → `--hdr_metal_kill`), the `(1-F)` diffuse-energy
asymmetry (F1), and the env probe capture (E0 → `--env_bake_linear`) are the same bug three times.
That is an unfinished migration, not three coincidences — so this is the exhaustive sweep.

**Method.** Every write to `lB/lG/lR`, `sB/sG/sR` and `fdB/fdG/fdR` in the scalar tile
(`Render_DeferredLighting_Tile`, the only path greets runs) was enumerated mechanically and asked
two questions: *(1) which composite was this expression written for?* and *(2) is it correct where
it currently lands?* The composite is `DeferredSurfaceKernel.cpp:2708`:

```cpp
float rlB = aB*aB*dlB + sB;      // aB = texB/255 ; dlB = lB after --hdr_metal_kill
```

Verdicts: **LINEAR-OK** (correct as it stands), **GAMMA-DEAD** (a gamma-composite term that lands
only on the discarded LDR `fdB` and therefore does nothing), **GAMMA-LIVE** (a gamma-composite term
that DOES reach the linear frame and is therefore wrong), **FIXED** (a former GAMMA-LIVE row now
behind a flag).

> Line numbers at `fog-wt` / `fac9732`. All "MEASURED" rows are runs made for this audit at greets
> `t=2000`, `--no-bloom`, dummy SDL drivers.

### 8.1 The diffuse / ambient accumulator `lB` — enters the frame as `albedo² · lB`

| # | term | line | verdict | note |
|---|---|---|--:|---|
| **M1** | emissive seed `Mat->Luminosity · 255` | `:1756` | **LINEAR-OK** | It rides the same `aB²` the composite applies, so the emissive is albedo-**squared** where the gamma composite gave it albedo¹. That is a real change of both magnitude and saturation made silently at the migration — but it is the correct linear form and the GPU does the same (`S.baseColor * S.lum`, `deferred.metal:786`; contract B5). Recorded so nobody re-opens it. |
| **M2** | **SH ambient `Mat->Diffuse · E(n)`** | `:1756`, source `EnvBake.cpp:2148-2200` | **GAMMA-LIVE → FIXED** by `--env_bake_linear` | **NEW, and it is the largest row in this table.** The scene-centre SH probe is projected from `renderSixFaces`' **8-bit face store** — the same store E0 indicts. By default that store is the LDR VPage combine `texel·light/256`, i.e. **gamma albedo at power 1**, so `E(n)` is a gamma irradiance being multiplied by `aB²` in a linear frame — E0's bug, in the term that touches **every pixel** rather than only conductors. **SPLIT 2026-08-08 into its own flag, `--sh_bake_linear`** (default OFF), because `--env_bake_linear` was doing two things at once and the halves had never been priced apart. **MEASURED:** `[SHAMB]` DC ambient B/G/R **148.3/158.8/154.5 → 107.8/116.7/118.3 (−27 %)** under `--sh_bake_linear`. **The claim in the sentence this replaces — that the ambient half is "most of" the flag's ~15-luma whole-frame move — is WRONG, and the split is what showed it:** at the greets projector pose the reflection half is **−9.64** luma and this ambient half **−5.44** of a **−15.27** total. See §9. |
| M3 | AO `lB *= ao` | `:1949` | LINEAR-OK (as a *migration* row) | The composite change did not alter its meaning. Its own defects — unclamped, can go negative, occludes the emissive — are D6/D6b and are unrelated to the migration. |
| M4 | vec-path light sums `lB += bufB[i]` | `:2200` | LINEAR-OK | light enters at power 1, which is the linear convention (L1). |
| M5 | direct diffuse `lB += intensity · Lcb` | `:2426` | LINEAR-OK | same. This is the row the squared-light-colour bug (§6.2d bug 1) was on the GPU side of; the CPU was already right. |
| M6 | `--ao_direct` `lB *= aoD` | `:2520` | LINEAR-OK, inert | default OFF. |
| M7 | the 250 clamp | `:2541` | **explicitly migrated** | `if (!hdrWrite)` — the author of the HDR path lifted it deliberately. Cited as the counter-example: when the migration was done consciously it was done right. |
| M8 | `--hdr_metal_kill` | `:2690-2707` | **FIXED** (default 2) | the D1 row. |

### 8.2 The specular accumulator `sB` — enters the frame **unsquared**, at power 1

| # | term | line | verdict | note |
|---|---|---|--:|---|
| S-a | direct GGX add | `:2502` | LINEAR-OK | `spec` is dimensionless × `Lcb` (0–255 light) → a radiance. |
| S-b | roughness-map `sB *= specMul` | `:2590` | LINEAR-OK | dimensionless magnitude scale. |
| **S-c** | **metal tint `sB *= 1-m + m·texB/255`** | `:2601` | **GAMMA-LIVE → FIXED (direct lobe only)** by `--metal_spec_f0` | `texB` is the **gamma** texel used as a linear reflectance. Contract D2. Now superseded on the direct lobe: `--metal_spec_f0` puts the **linear** albedo into F0 and skips this block. |
| **S-d** | **env metal tint `tB = 1-m + m·texB/255`** | `:1309-1311` (`EnvSpecComposeScalar`) | **GAMMA-LIVE → FIXED** by `--env_metal_tint_linear` | The identical gamma-albedo-as-linear-reflectance expression, on the **env** lobe — which is where essentially all of a conductor's appearance lives (with `--no-env_refl` a conductor renders at 0.00). `--metal_spec_f0` does **not** cover it. Fixed behind `--env_metal_tint_linear`, **default OFF**. It is a **TINT** fix, not a brightness one — see §9. |
| **S-e** | env probe content `sB += ecB·ek·tB` | `:1312` | **GAMMA-LIVE → FIXED** by `--env_bake_linear` | E0. |
| S-f | `Mat->SpecMul` | `:2638` | LINEAR-OK | dimensionless. |

### 8.3 The LDR combine `fdB` — **discarded** under `--hdr_linear`

Everything here is applied to `fdB` and then thrown away at `:2708`, which uses `lB`. These rows are
the migration's dead letters.

| # | term | line | verdict | note |
|---|---|---|--:|---|
| F-a | `fdB = texB·lB/256` | `:2562` | GAMMA-DEAD | the gamma composite itself. |
| F-b | metal diffuse kill `fdB *= dk` | `:2575` | GAMMA-DEAD, **re-landed** | the intent was re-expressed in the linear path as `--hdr_metal_kill`. The dead line is left in place because the LDR VPage path still ships in non-HDR configurations. |
| **F-c** | **`(1-F)` diffuse energy `fdB *= dc`** | `:2629` | **GAMMA-DEAD, NOT re-landed** | `--diffuse_energy` is **ON for greets** (`GREETS.CPP:1113`) and has **no effect whatsoever** on the shipped frame. Contract F1. Deliberately left alone here: re-landing it removes energy from diffuse in proportion to a Fresnel whose own reflection term (S-d, S-e) is still being corrected, so it should land **after** the env side is settled, not before. Recorded as owed. |
| F-d | water blend on `hB` | `:2666` | GAMMA-DEAD, **re-landed** | the linear path has its own at `:2712`, and it correctly squares the gamma underlay. Another consciously-migrated row. |

### 8.4 What the audit concludes

**Four instances of one bug, not three.** The known three (D1 / F1 / E0) plus **M2, the SH ambient
probe**, which is the biggest of them by pixel count and was hiding inside `--env_bake_linear`
without anybody saying so. Two rows remain **GAMMA-LIVE and unfixed**: **S-d** (the env lobe's metal
tint) and, if one counts a dead term as a defect, **F-c**.

**The rows that are RIGHT are now recorded**, so this sweep does not have to be repeated: M1, M3–M7,
S-a, S-b, S-f, F-d.

**Pattern worth keeping:** every row that was migrated *consciously* (M7's clamp, M8, F-d's water)
is correct. Every row that was migrated *implicitly*, by simply being upstream of the composite, is
correct only by luck (M1) or wrong (M2, S-c, S-d, S-e). The failure mode is not carelessness in the
math — it is that the composite changed underneath expressions that nobody re-read.

> **§8.4 update, 2026-08-08.** **S-d is now FIXED** behind `--env_metal_tint_linear` and **M2 has been
> split out** as `--sh_bake_linear`. Of the audit's GAMMA-LIVE rows only **F-c** remains, and it is a
> dead term (a `(1-F)` that never reaches the frame), not a live error. See §9 for the measurements.

---

## 9. S-d measured — it is a TINT, and the split of `--env_bake_linear`

All figures: greets `t=2000`, `FDS_GREETS_CAM="43.0,3.4,-62.85,1.0,0.0,0.0"` (the projector),
`--no-bloom`, dummy SDL drivers, **one** conductor mask — the `--hdr_metal_kill` 0-vs-2 change set at
that pose, **73,831 px** (3.56 % of the frame). Everything is read off the final tonemapped 8-bit
frame, so ACES + the sqrt encode are already in the numbers. §6.2l's 147,665-px mask is a different
definition and is not quoted here.

### 9.1 The tint, per channel

| arm | R | G | B | Y | chromaticity (R,G,B)/Σ | hue | sat |
|---|--:|--:|--:|--:|---|--:|--:|
| CPU default | 127.42 | 101.32 | 39.95 | 102.13 | 0.474 / 0.377 / 0.149 | 42.1° | 0.687 |
| CPU `--env_metal_tint_linear` | **127.42** | 88.75 | 20.37 | 92.52 | 0.539 / 0.375 / 0.086 | 38.3° | **0.840** |
| GPU (oracle) | 87.38 | 57.78 | 13.99 | 61.64 | 0.549 / 0.363 / 0.088 | 35.8° | **0.840** |

**R does not move at all.** `screen emiter` authors a constant albedo (255, 206, 104), so squaring the
normalised R is the identity. A brightness bug cannot leave one channel exactly fixed while moving the
other two by 12 % and 49 % — that is the signature that S-d is a per-channel defect, and it is why a
week of luma-only comparison did not see it. Chromaticity distance to the GPU falls **0.097 → 0.016**;
saturation lands on the GPU's **0.840 exactly**.

The same holds with the user's shadow arm: `--no-greets_omni_shadows` alone gives
129.73/103.53/39.95, and with the tint 129.73/90.82/20.37 — the tint fix is orthogonal to the shadow.

### 9.2 The §6.2k residual

| arm | conductor Y | ÷ GPU |
|---|--:|--:|
| CPU `--env_bake_linear` | 69.05 | **1.120×** |
| CPU `--env_bake_linear --env_metal_tint_linear` | 62.52 | **1.014×** |
| GPU | 61.64 | — |

**S-d absorbs almost all of the 1.121× residual on LUMA.** It does **not** close it on chroma: at
1.014× the CPU sits at (0.587, 0.344, 0.069) against the GPU's (0.549, 0.363, 0.088) — it now
*overshoots*. Cause, MEASURED and separate: the CPU's **linear-captured probe content is itself more
saturated than the GPU's**. Mean face B/G/R over the six `screen emiter` faces — CPU
`--env_bake_linear` **10.86 / 21.30 / 29.87** (Y 22.67) vs GPU **12.93 / 22.69 / 27.85** (Y 23.12).
The luma agrees to 2 % (as §6.2k reported) and the *colour* does not; nobody had compared the faces
per channel. A candidate mechanism, MEASURED as a fact but not as the attribution: **the CPU's
reflection probes contain no SH ambient at all** — the six faces are byte-identical under
`--sh_ambient` and `--no-sh_ambient`, because the reflection probes are baked before
`SHAmbient_EnsureBaked` runs, while `--no-sh_ambient` moves the main frame by −6.15 luma. The GPU's
probes do carry its ambient. Recorded as open.

> **CLOSED by §9.4 (2026-08-09).** The candidate above is confirmed as a real bug and fixed behind
> `--env_bake_sh_first` — but it is **only 14 % of the chroma gap**, and the rest is contract row
> **B2**: the two arms' ambients are *different quantities* (CPU = a neutral scene-centre room
> capture, DC 148.3/158.8/154.5; GPU = the FLD's blue-zenith sky gradient (0,40,80)→(100,80,60) ×
> 0.25). Read §9.4 before treating the ordering fix as the answer.

### 9.3 `--env_bake_linear` split in two

`renderSixFaces` feeds two consumers, and one flag drove both. The gate is now the caller's:
`--env_bake_linear` = the **reflection** probes, `--sh_bake_linear` = the **scene-centre SH ambient**
probe. Both default OFF.

| arm | whole-frame Y | conductor-mask Y | `[SHAMB]` DC B/G/R |
|---|--:|--:|---|
| neither | 101.43 | 102.13 | 148.3 / 158.8 / 154.5 |
| `--env_bake_linear` only | 91.79 | 69.05 | 147.7 / 158.1 / 153.8 |
| `--sh_bake_linear` only | 95.99 | **102.13** | 108.4 / 117.3 / 119.0 |
| both (the old combined flag) | 86.17 | 69.05 | 107.8 / 116.7 / 118.3 |

Two things the split settles. **(1) §8.1 M2's "most of the flag's move" was wrong**: reflections own
−9.64 of the −15.27 and the ambient −5.44. **(2) The conductor mask does not move under
`--sh_bake_linear` at all** — `--hdr_metal_kill=2` already removes the entire diffuse/ambient term
from a metalness-1 surface, so a conductor has no ambient to correct. That is a consistency check on
D1 as much as on this flag.

Images: `docs/img/metal/projector_users_arm_tint.png` — **the user's own arm**:
`--no-greets_omni_shadows` | the same **+ `--env_metal_tint_linear`** | GPU.
`docs/img/metal/projector_env_metal_tint.png` (default | tint | GPU),
`docs/img/metal/projector_env_metal_tint_2rows.png` (the same, second row with `--env_bake_linear`),
`docs/img/metal/greets_bake_linear_2x2.png` (neither | probe | ambient | both).

---

## 9.4 The probe-colour residual, run upstream — it is TWO things, and only one is a CPU bug

§9.2 left this open: with `--env_bake_linear --env_metal_tint_linear` the
conductor's LUMA lands at 1.014× the GPU's but the CHROMA overshoots, because the
CPU's linear-captured probe **content** is itself more saturated than the GPU's
(mean face B/G/R **10.86/21.30/29.87** vs **12.93/22.69/27.85**). All figures
below are the same pose and the same 73,831-px mask as §9.1, and they reproduce
§9.1's whole table exactly (CPU default 127.42/101.32/39.95 sat 0.687, GPU
87.38/57.78/13.99 sat 0.840), so they are directly comparable.

### 9.4a Cause 1, a CPU BUG, and it is an ORDERING one — fixed behind `--env_bake_sh_first`

**The CPU's reflection probes contain the FLAT `Sc->Ambient` constant where the
shipped frame contains the coloured SH irradiance.** Mechanism, read out of the
source and confirmed by measurement:

| | |
|---|---|
| `RENDER.CPP:491` | `EnvReflection_FramePrep` — the reflection probes |
| `RENDER.CPP:511` | `SHAmbient_EnsureBaked` — the scene-centre SH probe |

Same frame, that source order. `SHAmbient_Coeffs` (`EnvBake.cpp:2135`) returns
null until `SHProbe::baked` is set at `:2223`, so **every reflection-probe face
is shaded through the kernel's flat-ambient fallback**
(`DeferredSurfaceKernel.cpp:1799`, `lB = Lum·255 + Mat->Diffuse · Sc->Ambient.B`)
rather than its SH branch (`:1789-1793`). Greets authors that constant as
**(32, 32, 32)** — an **achromatic** grey. The GPU's probe bake, by contrast,
runs the frame's own `fs_lighting` with `shBuf` bound (`Deferred.mm:2153-2156`),
so `AmbientRadiance` (`deferred.metal:785`) is live in every probe texel.

**The order is incidental, not a dependency.** `SHAmbient_EnsureBaked` reads
nothing the reflection bake produces — it derives its own scene AABB
(`EnvBake.cpp:2153`) — and the two anti-recursion guards (`g_envBakeInProgress`,
`g_offscreenViewDepth`) are symmetric, so either order is legal. `--env_bake_sh_first`
(`FeatureFlags.def`, **default 0, byte-null**, inert unless both `--sh_ambient`
and `--env_refl` are on) calls the SH bake from the top of
`EnvReflection_FramePrep`. One bake either way; no extra render.

**MEASURED, probe content** (`FDS_ENVBAKE_DUMP=1`, `screen emiter`, mean over the
six faces; the GPU column is `--dump_env_cube`, ×255 onto the same 0–255 scale):

| arm | B | G | R | Y | chromaticity (R,G,B)/Σ | distance to GPU |
|---|--:|--:|--:|--:|---|--:|
| CPU `--env_bake_linear` | 10.86 | 21.29 | 29.87 | 22.67 | 0.482 / 0.343 / 0.175 | 0.053 |
| CPU **+ `--env_bake_sh_first`** | 12.31 | 23.54 | 32.38 | 24.91 | 0.475 / 0.345 / 0.180 | **0.045** |
| GPU (oracle) | 12.93 | 22.69 | 27.85 | 23.12 | 0.439 / 0.358 / 0.204 | — |

**MEASURED, the lit conductor mask** (73,831 px):

| arm | R | G | B | Y | chromaticity | distance to GPU |
|---|--:|--:|--:|--:|---|--:|
| `--env_bake_linear --env_metal_tint_linear` | 95.18 | 55.85 | 11.22 | 62.52 | 0.587 / 0.344 / 0.069 | 0.047 |
| **+ `--env_bake_sh_first`** | 100.25 | 59.90 | 12.22 | 66.53 | 0.582 / 0.348 / 0.071 | **0.040** |
| GPU | 87.38 | 57.78 | 13.99 | 61.64 | 0.549 / 0.363 / 0.088 | — |

**Verdict, and it is deliberately unflattering to the fix.** It is a real defect
and the fix is correct by the CPU's own standard — the probe should hold the same
ambient the frame does. But it buys **17 %** of the probe-chroma gap and **14 %**
of the mask-chroma gap, and it costs luma: the conductor ratio goes **1.014× →
1.079×**. **It does not close the tint gap.** Anyone reading §9.2's "recorded as
open" as "this is the answer" would be wrong, and this section exists to say so.

Cost stated: with the SH probe baked first, the SH probe's own faces no longer
see the reflection probes' output. **MEASURED as −0.6 %:** `[SHAMB]` DC ambient
B/G/R **148.3/158.8/154.5 → 147.5/157.9/153.4** — the same tiny reverse coupling
already recorded on `--sh_bake_linear` as −0.4 %.

Note it is not a small-coverage flag: it moves **1,315,524 px (63.4 %)** of the
greets frame and whole-frame luma **101.07 → 103.02**, because greets is largely
reflective (`--env_bake_linear` alone moves 57.0 %).

### 9.4b Cause 2, NOT a CPU bug — the two arms' ambients are DIFFERENT QUANTITIES

This is contract row **B2**, and §9.4a is what makes it visible in probe content
for the first time. Both arms' ambient sources, measured:

| arm | SH source | measured |
|---|---|---|
| CPU | a 32²×6 cube capture at the scene AABB centre — **the room** | `[SHAMB]` DC B/G/R **148.3 / 158.8 / 154.5** → RGB ≈ (155, 159, 148), effectively **NEUTRAL** |
| GPU | the FLD's authored **sky gradient**, analytically projected (`Deferred.mm:263`), × `ambientFactor` 0.25 | `[FLD] scene sky gradient: zenith=(0,40,80) nadir=(100,80,60)` — a **deep-blue** zenith |

So the GPU's probes carry a **blue-leaning** ambient and the CPU's a neutral one,
and that is exactly the direction of the residual: the GPU's probe chromaticity
has **B = 0.204** against the CPU's **0.175 / 0.180**. **No ordering change on
the CPU can add blue that the CPU's own ambient does not contain.** B2 and B4 had
recorded the source and normalisation divergence; what is new here is that it
**propagates into probe content**, which is why the tint fix overshoots on chroma
while agreeing on luma.

**Where that leaves "still no gpu parity".** The colour gap decomposes into: the
env-lobe tint (§9.1, fixed, `--env_metal_tint_linear`, saturation 0.687 → 0.840,
GPU's 0.840 exactly); the probe's missing ambient (§9.4a, fixed,
`--env_bake_sh_first`, 14 % of what is left); and **the two arms disagreeing
about what the ambient IS** (§9.4b), which is a scope divergence in the GPU arm
and cannot be closed from the CPU side. A fourth candidate remains unpriced and
is now the leading one: **E3b**, the CPU's padded 102.68° software faces with
face-major bilinear against the GPU's plain 90° hardware `texturecube` — note the
two censuses above are not even integrating the same solid angle.

Image: `/Users/gil-ad/work/revival-fog/docs/img/metal/decision_env_bake_sh_first.png`

Reproduce (literal, from `Runtime/`):

```sh
cd Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_ENVBAKE_DUMP=1 \
  FDS_GREETS_CAM="43.0,3.4,-62.85,1.0,0.0,0.0" ./DEMO --snapshot=greets@t=2000 \
  --out=/tmp/gr --deferred --hdr --glass-refract=1 --glass-test \
  --xpar-peel-passes=4 --profiler=0 --no-bloom --env_bake_linear --env_bake_sh_first
```

---

## 9.5 DEFAULT RECOMMENDATION for each conductor flag — HIS call, with a picture and a command each

Every command below is literal and runs from `Runtime/` with dummy SDL drivers.
Every image is a full path. The mask is §9.1's 73,831 px.

### `--hdr_metal_kill=2` — **KEEP ON.** Already shipped, already reviewed.

Nothing new. It is the reason `--no-env_refl` renders a conductor at 0.00, i.e.
the reason the env lobe is the whole argument.
Image: `/Users/gil-ad/work/revival-fog/docs/img/metal/decision_hdr_metal_kill.png`

```sh
cd Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  FDS_GREETS_CAM="43.0,3.4,-62.85,1.0,0.0,0.0" ./DEMO --snapshot=greets@t=2000 \
  --out=/tmp/gr --deferred --hdr --glass-refract=1 --glass-test \
  --xpar-peel-passes=4 --profiler=0 --no-bloom --hdr_metal_kill=0
```

### `--env_metal_tint_linear` — **RECOMMEND ON.**

The strongest of the four. It is the last GAMMA-LIVE row of the `--hdr_linear`
migration audit (§8.2 S-d), it is a per-channel defect and not a brightness one
(**R does not move at all**, because `screen emiter` authors albedo (255,206,104)
and squaring the normalised R is the identity), and it lands the conductor's
saturation on **0.840, the GPU's value exactly**. Chromaticity distance to the
oracle **0.097 → 0.016**. Whole-frame luma moves **101.07 → 100.53**, 11.1 % of
pixels. **Byte-pin consequence:** greets `6780642b` moves; fountain `8db68ccb`
and city `5476be8c` do NOT (no metallic-mapped material).
Image: `/Users/gil-ad/work/revival-fog/docs/img/metal/decision_env_metal_tint_linear.png`

```sh
cd Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  FDS_GREETS_CAM="43.0,3.4,-62.85,1.0,0.0,0.0" ./DEMO --snapshot=greets@t=2000 \
  --out=/tmp/gr --deferred --hdr --glass-refract=1 --glass-test \
  --xpar-peel-passes=4 --profiler=0 --no-bloom --env_metal_tint_linear
```

### `--metal_spec_f0` — **RECOMMEND OFF**, and this reverses nothing: it was already OFF.

Physically right and visually absent. MEASURED at the momy pose (its only live
target — it moves **0 px** at the projector, for §10's shadow reason):
**22,364 px, 1.08 % of the frame**, mean signed ΔRGB **+0.02 / +0.63 / +1.46**,
**max single-pixel delta 8/255**. Against that, it moves the CPU *away* from the
current oracle — the GPU uses the same dielectric F0 (`deferred.metal:575`).
Turning it on buys a byte-pin move for a change nobody can see. **Land it when
the GPU gets the same fix, as a pair.**
Image (the third panel is |difference| ×16, because at ×1 there is nothing to
show): `/Users/gil-ad/work/revival-fog/docs/img/metal/decision_metal_spec_f0.png`

```sh
cd Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  FDS_GREETS_CAM="12.0,3.0,-29.0,0.0,-0.0875,-0.9962" ./DEMO --snapshot=greets@t=2000 \
  --out=/tmp/gr --deferred --hdr --glass-refract=1 --glass-test \
  --xpar-peel-passes=4 --profiler=0 --no-bloom --metal_spec_f0
```

### `--shadow_noncaster_depth` — **RECOMMEND ON, but it is a LOOK decision and it is a big one.**

The correctness case is closed (§10): the recovered direct term is **bit-identical
to removing the shadows entirely**, so it recovers 100 % of what the identity test
was eating and nothing more, and the GPU's own ray-cast ground truth agrees. The
scale is what makes it his call, not mine: **489,567 px (23.6 %)** of the frame,
whole-frame luma **101.07 → 104.72**. In the image the pillars stop being
uniformly black-shadowed and take the amber omnis — that is not a subtle
re-grade, it is a re-lighting of every non-casting material in the scene. On the
conductor mask itself it is small (**102.13 → 103.14**). **Byte-pin
consequence:** greets `6780642b` moves. Fountain and city need checking before it
lands — any scene with a `lamp`/`emi`-named or transparent/additive material is
in scope, which is not greets-only the way the tint is.
Image: `/Users/gil-ad/work/revival-fog/docs/img/metal/decision_shadow_noncaster_depth.png`

```sh
cd Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  FDS_GREETS_CAM="43.0,3.4,-62.85,1.0,0.0,0.0" ./DEMO --snapshot=greets@t=2000 \
  --out=/tmp/gr --deferred --hdr --glass-refract=1 --glass-test \
  --xpar-peel-passes=4 --profiler=0 --no-bloom --shadow_noncaster_depth
```

### `--env_bake_sh_first` (new, §9.4a) — **RECOMMEND OFF for now.**

Correct by the CPU's own standard, but it buys 14 % of the chroma gap, costs 6.5 %
on luma in the wrong direction, and moves 63 % of the greets frame. Its natural
home is **with** `--env_bake_linear`, which is itself still OFF pending review —
and `--env_bake_linear` is the flag that owns the brightness half of the same
problem. Land the two together or neither.

### One item that is REPORTED and NOT FIXED, and it bites whoever flips these

`DEMO/CITY.CPP`'s env-cube cache salt hashes an explicit list of bake-affecting
flags. `--env_metal_tint_linear`, `--sh_bake_linear`, `--shadow_noncaster_depth`
and now `--env_bake_sh_first` are **not** in it, so a city run with any of them on
hits a cache baked without them. Inert at the defaults; that file is not this
arm's to edit.

---

## 10. The projector's dead direct term — PolyId, not bias, not the cage

§6.2l left this open: `screen emiter`'s direct term is **exactly zero** and only
`--greets_omni_shadows` moves it. **Root cause, MEASURED:**

`RENDER/Shadows.cpp` excludes a material from the shadow **bake** when it is
`Mat_Transparent|Mat_Additive|Mat_SkipZ` or its **name** contains `"lamp"`/`"emi"` — the heuristic that
stops a lamp housing occluding the omni inside it. The source comment there already lists the
casualties by name: *"name-hit but NOT Luminous: 'lamp', 'screen emiter fance', 'screen emiter'"*.

Greets runs `ShadowMode::PolyId`, where a cube tap is occluded iff the stored id is non-zero **and
differs from the receiver's own id** — an identity test, "the closest thing to the light along this ray
must be ME". **A material that was never rasterised into the cube can never satisfy it.** Whatever the
bake *did* store along that ray — the room behind the surface — reads as an occluder, so a non-casting
material is shadowed **for ever, by every shadow-casting omni**.

Isolated with `--no-env_refl`, which leaves only the direct term on a conductor (73,831-px mask,
per channel R/G/B):

| arm | R | G | B | Y |
|---|--:|--:|--:|--:|
| default | 0.000 | 0.000 | 0.000 | 0.000 |
| `--shadow_noncaster_depth` | 0.397 | 0.348 | 0.000 | 0.323 |
| `--no-greets_omni_shadows` | 0.397 | 0.348 | 0.000 | 0.323 |
| `--no-shadows` | 0.397 | 0.348 | 0.000 | 0.323 |

**Bit-identical to removing the shadows entirely** — the fix recovers 100 % of the lost direct term
and nothing more. Full shipping stack: conductor mask 102.13 → 103.14 (`--no-greets_omni_shadows`
gives 104.11, the extra being other objects' real shadows), whole frame 101.43 → 105.06, **487,634 px
(23.5 %) changed** — every non-casting material in the scene, not just the projector.

**The other three hypotheses are dead.** The cage does *not* occlude its own contents:
`screen emiter fance` matches `"emi"` too and is equally excluded from the bake. It is not a bias or
near-plane problem: with the identity test replaced by the **existing** biased depth compare the term
returns in full. It is not face selection: the same tap, same face, different test.

**A measurement trap that produced a wrong verdict, recorded so it is not repeated.**
`--no-shadow_polyid` **on the command line does nothing**. `g_shadowMode` is a namespace-scope dynamic
initialiser (`Shadows.cpp`) that reads the flag *before* `main()` parses argv; only the env form
`FDS_SHADOW_POLYID=0` (the flag table's eager env scan does see it) or the F3 toggle can move it.
§6.2l's "`--no-shadow_polyid` changes nothing, so this is not PolyId" was measuring a flag that never
took effect — with the env form, Depth mode recovers the whole direct term.

**Verdict: the CPU is wrong and the GPU is right**, agreeing with the GPU's own ray-cast ground truth
(`--probe`: lights 5 and 6 `RAYCAST: clear`). Fixed behind **`--shadow_noncaster_depth`**, default OFF:
a receiver the caster predicate excludes resolves its `surfaceShadowId` to −1, the already-documented
"force Depth semantics" sentinel of `resolveCubeAtten` / `CubeShadow_Sample`, and also skips the static
shadow lightmap (whose atlas was baked through the same identity test). Depth acne is not a risk for
such a surface: acne comes from comparing a surface against its **own** stored depth, and this one
never wrote any. Image: `docs/img/metal/projector_noncaster_depth.png` (top row full stack, bottom row
`--no-env_refl` direct-only; bottom-right is where the two GGX highlights appear).

**Reported, not fixed:** `DEMO/CITY.CPP`'s env-cube cache salt hashes an explicit list of
bake-affecting flags. `--env_metal_tint_linear`, `--sh_bake_linear` and `--shadow_noncaster_depth` all
change what the city bake renders and are **not** in that list, so running city with any of them on
would hit a cache baked without them. Inert at the defaults; that file is not mine to edit.

---

## 11. THE MECH — why the two arms disagree on `cockpit` / `hull` / `canons`

Raised by the user flying greets: *"the mech's metallic look differs, especially the specular
highlights"*, and later *"the mech in the gpu looks much better … I'm just trying to understand the
discrepancy."* This section is the **explanation**, not a recommendation. Every number below is
MEASURED at his own pose unless labelled otherwise. Nothing in `FDS/` or `DEMO/` was changed.

The pose, used verbatim by every command in this section:

```
cd Runtime
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" FDS_GREETS_FOV=58.1092072 ./DEMO --snapshot=greets@t=4871 --out=/tmp/m_cpu_base --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ../build-gpu/GpuBench/GpuBench --fld=SCENES/GREETS.FLD --t=4871 --cam="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" --pass=deferred --xres=1920 --yres=1080 --out=/tmp/gpu_probe.ppm --iters=1 --warmup=0
```

### 11.1 The inputs, per material — what each arm READS

Sources: CPU `[MAT-TABLE]` (`--dump_mats`), CPU `[ENVDBG]` (`ENVDBG=1`, `EnvBake.cpp:1261`), CPU
`[MAT-IMPORT]` / `[GREETS] baked tangent-space normal map` lines, GPU `[ENVREFL]` census
(`SceneIngest.cpp:1711`) and `[INGEST] revmap` lines (`SceneIngest.cpp:420`).

| material | texture | Diff | Spec | Refl | Gloss | RVSM albedo/normal/rough/metal/AO | CPU normal map | GPU normal map |
|---|---|--:|--:|--:|--:|---|---|---|
| `cockpit` | MECH_COK.JPG | 1.00 | 1.00 | **50.00** | 64 | **none** | **Sobel-from-diffuse** | **none** |
| `hull not smooth` | MECH_HUL.JPG | 1.00 | 0.40 | 0.00 | 48 | **none** | **Sobel-from-diffuse** | **none** |
| `hull` | MECH_HUL.JPG | 1.00 | 0.40 | 0.00 | 48 | **none** | **Sobel-from-diffuse** | **none** |
| `canons` | MECH_HUL.JPG | 1.00 | 0.00 | 0.00 | 0 | **none** | none | none |
| `screen emiter` | PELLOW.JPG | 0.67 | 0.62 | 0.00 | 128 | albedo, normal, rough, **metal** | authored PNG | authored PNG |

**No mech material carries an RVSM PBR map of any kind.** The registry has exactly seven entries —
`momy-1`, `momy-2`, `amudim`, `screen emiter`, `stairs`, `rooms`, `teleporter` — and not one is a mech
surface (MEASURED, both arms' logs agree: CPU 32 maps applied, GPU 32 maps applied, same seven
materials). So "the metalness map sets the look" is a rule with **no input to act on** here. On both
arms every mech material has `metalness = 0` at every pixel, which is why `--metal_spec_f0` moved
**0 px** and `--env_metal_tint_linear` moved **4 px** over the mech: those flags only touch conductors.

`Runtime/TEXTURES/MECH_RM.JPG` exists (256², flat grey, mean 136/136/136) and GREETS.FLD names it on
the `cockpit` surface in the slot immediately before `MECH_COK.JPG` — a 1998-era spherical
**reflection map**, not a roughness/metalness map. **Neither arm loads it** (no `[MAT]`/`[INGEST]`
line mentions it in either log). Stale authoring data, inert on both sides.

### 11.2 The env-qualification decision, per material — both arms AGREE

CPU gate `DeferredSurfaceKernel.cpp:1904` and `EnvBake.cpp:1218`: `env_refl && (Reflection > 0 || MetallicMap)`.
GPU gate `SceneIngest.cpp:1671`: `(b.reflection > 0.0f) || (b.metalTexIndex >= 0)`. **Same rule, same
inputs, same outcome.** Measured, per material:

| material | CPU `[ENVDBG]` | GPU `[ENVREFL]` | qualifies? |
|---|---|---|---|
| `Hull.lwo::cockpit_upper::mirUV` | `refl=50 metal=0` → probe at (−3.5, 2.3, −30.0) | `'cockpit' (Reflection=50.00)` → probe 2 at (−3.4, 2.3, −30.0) | **YES, both** — `Reflection > 0` |
| `Hull.lwo::cockpit_body::mirUV` | `refl=50 metal=0`, centroid (−3.3, 2.2, −29.9) | same probe (same material name) | **YES, both** — shares the store, 0.24 u apart, under the 4-u dedup |
| `hull`, `hull not smooth`, `canons` | **absent from the dump** | **absent from the census** | **NO, both** — `Reflection = 0`, no metalness map |
| `screen emiter` | `refl=0 metal=1` | `'screen emiter' (Reflection=0.00, metallic map)` → probe 5 | **YES, both** — qualified **by the map alone** |

`screen emiter` is the clean demonstration that "a metalness map qualifies a surface regardless of
`Reflection`" is **already the rule on both arms**. `hull`/`canons` do not reflect on **either** arm —
the GPU's richer look on them is not an env reflection. Confirmed independently: `--no-env_refl` on
the GPU changes **33,478 px, all inside x[569..945] y[551..699]** — the canopy and nothing else.

So the leading hypothesis (the GPU qualifies on a metalness map where the CPU demands `Reflection > 0`)
is **DEAD, measured**. The difference is four other things.

---

### E6 — the CPU keeps **every ANIMATED mesh** out of every env probe; this arm keeps none out

| | CPU | GPU |
|---|---|---|
| rule | `EnvBake.cpp:311` sets `g_envBakeSkipDynamic = true` for every ordinary probe bake; `Transform.cpp:1274` folds it into `inStaticBake`; `Transform.cpp:1559` `continue`s past every mesh `isDynamicForBake` calls dynamic (Pos-spline extent > 0.1 u, or Rotate quaternion extent > 0.01, on the object **or any ancestor**) | nothing — every batch is drawn into every probe (`Deferred.mm` `bakeEnvProbes`), only the probe's own material is excluded |
| stated rationale | `Transform.cpp:1265` — *"the panorama is a STATIC capture, so moving meshes (the walking mech) must not be frozen into it — they'll have walked away by the time the reflection is seen"* | one-bounce static capture, no motion model |
| on greets | excludes `Hull.lwo`, `Hull2.lwo`, `L_leg1/2.lwo`, `R_leg1/2.lwo` — **the entire mech**, named in the log's `[STATIC-BAKE-SKIP-MESH]` lines (which are capped at 32 emissions, so the line COUNT is not an exclusion count — the six distinct mesh names are) | includes all of it |

**Verdict: DIVERGE, and it is the largest CONTENT difference between the two arms' probes.** The
CPU's `cockpit` probe holds the **empty room**; this arm's holds the mech's own hull, barrels and
legs. Side-by-side face atlases: `/tmp/atlas_cpu_big.png` (CPU, no mech anywhere) vs
`/tmp/atlas_gpu_big.png` (GPU, mech in five of six faces).

PRICED with the new `--env_bake_skip_animated` (GpuBench, **default OFF**, so every pinned md5 is
unchanged — verified byte-identical, md5 `d3a8301a22495c80dfdd5c3f8509f771` before and after the
build): **5,268 px changed, all inside x[569..929] y[551..695], mean |Δ| 24.94, max 126.**
Probe face means rise 47.91 → 52.22 on the 0–255 linear radiance scale.

### E7 — the same roughness selects a **3× wider lobe** on the GPU, because the mip chains differ in DEPTH

| | CPU | GPU |
|---|---|---|
| store | face res from `env_bake_res` (**256** for `cockpit`), chain **fixed at `EnvPanoLinear::kMaxMips = 4`** (`EnvBake.h:63`) → 256/128/64/32 | face res `--env_res` (**128** default), `textureCubeDescriptor … mipmapped:YES` + `generateMipmaps` → the **FULL** chain, 8 levels (128…1) |
| level select | `lvlF = rough * (numMips − 1)` (`DeferredSurfaceKernel.cpp:1109`) | `lvl = rough * (nMips − 1)` (`deferred.metal:714`) |
| `cockpit`: gloss 64 → `rough = sqrt(2/66) = 0.1741` | `lvlF = 0.1741 × 3 = 0.522` → between 256² and 128², effective **≈178²** | `lvl = 0.1741 × 7 = 1.219` → between 64² and 32², effective **≈55²** |

**The formula is identical; the divisor is not.** `rough = 0.174` means "half a level into a 4-level
chain" on the CPU and "1.2 levels into an 8-level chain" on the GPU. Two compounding factors — base
face res 256 vs 128, and chain depth 4 vs 8 — leave the GPU sampling the environment at roughly
**one third** the CPU's angular resolution. That is the mechanism behind *"the GPU melts the canopy's
window-frame ribs into a smooth mirror sweep"*.

CONFIRMED by sweep. High-pass RMS (7×7 box, luma) over the 33,310-px `cockpit` mask:

| render | effective face res | high-pass RMS |
|---|--:|--:|
| GPU `--env_res=128` (default) | ≈55² | **9.97** |
| GPU `--env_res=256` | ≈98² | **11.02** |
| GPU `--env_res=512` | ≈173² | **12.45** |
| CPU (checkerboard off, see P2 below) | ≈178² | **13.20** |

At matched effective resolution the two arms' canopy detail agrees to **6 %**. E3b said "DIVERGE
(unpriced)"; this prices it, and identifies the mip-chain **depth** — not the padding or the face
geometry — as the dominant half.

### 11.3 Walk three pixels through both arms

Material identified by GpuBench's host ray-cast (`--probe_px=X,Y`, which names the nearest hit's
mesh/material), so the attribution is ground truth and not inferred from colour.

| px | material | CPU shipped | CPU `--nmap_strength=0` | GPU |
|---|---|--:|--:|--:|
| (760, 620) | `Hull.lwo/cockpit` — canopy | Y 146.4, BGR (194,173,76) | 157.1 | 161.4, BGR (174,174,132) |
| (767, 723) | `Hull.lwo/hull` — hull highlight | Y **131.2**, BGR (37,146,138) | **45.0**, BGR (35,53,33) | **41.0**, BGR (36,48,29) |
| (780, 795) | `Hull.lwo/hull` — gun barrel | Y 108.4 | 110.1 | 82.6 |
| (560, 845) | `Hull.lwo/canons` | Y 76.6 | 76.6 | 94.6 |

Row 2 is the whole story of the hull: **one input differs and it is the normal map.** Both arms read
the same albedo, the same `Specular = 0.40`, the same `Gloss = 48` → `rough = sqrt(2/50) = 0.2`, and
neither applies an env lobe. Neutralise the CPU's Sobel map and the pixel goes 131.2 → 45.0, landing
4.0 luma from the GPU's 41.0.

### E8 — the CPU bakes a **normal map out of the DIFFUSE** for the mech; this arm has none

`DEMO/GREETS.CPP:1948-1970`: every material whose name contains `stairs`, `amudim`, `floor`, `marb`,
`MARB`, **`hull`** or **`cockpit`**, plus `rooms`/`siling`, gets
`M->NormalMap = BakeNormalMapFromDiffuse(M->Txtr, 4.0f)` **if it does not already have one**. On greets
that fires for exactly four materials (log lines, MEASURED): `cockpit`, `hull not smooth`, `hull`,
`siling` — the others already carry authored/RVSM normals. `BakeNormalMapFromDiffuse`
(`DEMO/MeshOps.cpp:1079`) box-blurs the **luminance** of the albedo `--nmap_blur` times (default 4)
and Sobels it. On MECH_HUL.JPG that is camouflage paint: **dark green blotches become geometric
dents.** GpuBench replicates none of it — `SceneIngest.cpp:1409` takes `M->NormalMap` only if an RVSM
or sidecar set one, and no mech material has either.

Recorded while reading it: the caller passes `4.0f`, and the function **ignores the argument** —
`MeshOps.cpp:1162` does `const float effStrength = fds::FeatureFlags::nmap_strength(); … (void)strength;`.
The strength that actually ships is the flag default, **1.5**.

PRICED with `--nmap_strength=0` (which zeroes the Sobel gradient — a flat tangent-space map, i.e. no
map — while leaving every **authored** normal map, the wall/floor stone included, untouched):
**311,080 px changed frame-wide (15.0 %).** It is concentrated, not diffuse: over the 143,343-px
`hull`+`hull not smooth` mask the mean |ΔY| is only **2.33** (p50 1.00), but **3.3 % of pixels move
more than 10 luma and the maximum is 162.7.** That is the precise shape of the user's report — *not*
a brightness difference, a **highlight-character** difference: the bump map is shredding the specular
lobe exactly where the lobe is bright and doing nothing elsewhere.

### 11.4 The canopy, term by term

Over the 33,310-px `cockpit` mask (mask built rigorously as "pixels where the GPU's env term is
non-zero", from a `--no-env_refl` A/B):

| render | Y mean | std | high-pass RMS | B/G/R |
|---|--:|--:|--:|---|
| CPU shipped | 143.73 | 28.29 | 18.54 | 153.98 / 155.33 / 117.06 |
| CPU `--nmap_strength=0` | 144.87 | 28.59 | 16.24 | 154.78 / 157.11 / 117.06 |
| CPU + `--env_bake_linear=1` | **114.08** | 31.23 | 18.12 | 110.68 / 124.71 / 94.51 |
| CPU + also `--deferred_checkerboard=0` | 107.75 | 29.36 | **13.20** | — |
| GPU default | 121.80 | 36.65 | 10.38 | 117.56 / 129.19 / 108.91 |
| GPU `--env_bake_skip_animated` | 124.39 | 34.37 | 9.97 | 119.02 / 131.45 / 112.58 |
| GPU + `--env_res=512` | 124.06 | 35.72 | **12.45** | — |

**E0 dominates the brightness and the colour cast.** Probe face census, same probe, both arms, on the
CPU's own 0–255 linear radiance scale (CPU: `FDS_ENVBAKE_DUMP=1` `[ENVBAKE-FACE]`; GPU:
`--dump_env_cube`):

| face | CPU shipped | CPU `--env_bake_linear` | GPU default | GPU `--env_bake_skip_animated` |
|---|--:|--:|--:|--:|
| +X | 96.54 | 42.01 | 43.90 | 47.47 |
| −X | 92.79 | 36.10 | 32.96 | 35.92 |
| +Y | 122.94 | 75.84 | 102.64 | 102.74 |
| −Y | 96.22 | 33.14 | 25.93 | 35.92 |
| +Z | 105.59 | 47.92 | 46.08 | 51.94 |
| −Z | 87.80 | 32.99 | 35.92 | 39.30 |
| **mean** | **100.31** | **44.67** | **47.91** | **52.22** |

The CPU's shipped probe is **2.09×** the GPU's — E0's `255/albedo` signature, and larger here than
§6.2k's 1.82× because the reflected room is the dark stone floor. It is a **per-channel** error, not a
gain: gamma storage lifts the darkest channel most, so the blue-poor brown room comes back
blue-dominant (`B/R = 2.27` on +Y vs the GPU's 1.39). That is why the shipped CPU canopy reads as pale
frosted blue and the GPU's reads as polished glass over a warm floor. With both fixes applied the
probes agree to **1.17×**, the residual being the +Y (ceiling) face — the already-documented
ambient-source divergence (B2 / §9.4b), not a new term.

**P2 is a third of the CPU's apparent "detail".** `deferred_checkerboard` is ON for greets
(`GREETS.CPP:1183`): half the canopy's pixels are reconstructed by the fill wave, not shaded, and the
cross-hatch is plainly visible at 2× zoom. Turning it off takes the CPU's high-pass RMS from 18.12 to
**13.20** — i.e. **4.92 of the CPU's excess high-frequency energy is the shading pattern, not
reflected content.**

### 11.5 What is left after all of it

Over the whole 300,352-px mech mask (built from the GPU's `--viz=worldpos` decode, box
x∈[−5.2,−1.8] y∈[0.6,4.2] z∈[−31.5,−27.5]):

| render | Y mean | signed ΔY vs GPU | mean \|ΔY\| vs GPU |
|---|--:|--:|--:|
| CPU shipped | 86.27 | +2.87 | 11.76 |
| CPU `--nmap_strength=0` | 86.55 | +3.15 | 11.43 |
| CPU + `--env_bake_linear=1` | 83.02 | **−0.37** | **10.28** |
| GPU | 83.40 | — | — |

Split by material: on the `hull`+`hull not smooth` mask the two arms already agree in the mean to
**+0.89** luma before any change; on the remainder (`canons` and the untextured mech parts) to
**+0.03**. **There is no systematic brightness disagreement anywhere on the mech.** The whole
complaint lives in the four rows above — E6, E7, E8 and E0 — plus P2, all of which move *where* light
sits, not how much of it there is.

### 11.6 Summary of the five mechanisms, ranked by what they do to the eye

| # | mechanism | which arm is different | measured size |
|---|---|---|---|
| **E8** | Sobel-baked-from-diffuse normal map on `cockpit`/`hull`/`hull not smooth` | CPU has it, GPU has none | 311,080 px frame-wide; 3.3 % of hull pixels > 10 luma, max 162.7; hull pixel 131.2 → 45.0 |
| **E0** | probe content stored as gamma VPage, added into a linear accumulator | CPU (documented, fix exists behind `--env_bake_linear`) | probe faces **2.09×**; canopy Y 143.73 → 114.08 |
| **E7** | env mip-chain depth 4 (CPU) vs full 8 (GPU) for the same `rough` | both — different divisors | ≈3× lobe width; canopy high-pass RMS 9.97 vs 13.20, closing to 6 % at matched res |
| **E6** | animated meshes excluded from probes | CPU excludes, GPU includes | 5,268 px, mean \|Δ\| 24.94, max 126 |
| **P2** | half-rate checkerboard shading | CPU only | 4.92 of the CPU's canopy high-pass RMS |

**Images** (absolute paths):

* `/tmp/mech_explain_strip.png` — mech crop, four panels: **A** CPU shipped, **B** CPU + `--nmap_strength=0 --env_bake_linear=1`, **C** GPU + `--env_bake_skip_animated`, **D** GPU default.
* `/tmp/canopy_explain_strip.png` — canopy at 2×, four panels: **A** CPU shipped, **B** CPU + `--nmap_strength=0 --env_bake_linear=1 --deferred_checkerboard=0`, **C** GPU + `--env_bake_skip_animated --env_res=512`, **D** GPU default. B and C are the same look; A and D are the two ends the user was comparing.
* `/tmp/atlas_cpu_big.png`, `/tmp/atlas_gpu_big.png` — the `cockpit` probe's six faces on each arm.

Literal commands for each panel, run from `Runtime/` (no shell variables — `--strict_flags` aborts on
an unsplit token):

```
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" FDS_GREETS_FOV=58.1092072 ./DEMO --snapshot=greets@t=4871 --out=/tmp/m_cpu_base --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" FDS_GREETS_FOV=58.1092072 ./DEMO --snapshot=greets@t=4871 --out=/tmp/cv_ebl --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --nmap_strength=0 --env_bake_linear=1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_GREETS_CAM="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" FDS_GREETS_FOV=58.1092072 ./DEMO --snapshot=greets@t=4871 --out=/tmp/cv_nocb --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --nmap_strength=0 --env_bake_linear=1 --deferred_checkerboard=0

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ../build-gpu/GpuBench/GpuBench --fld=SCENES/GREETS.FLD --t=4871 --cam="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" --pass=deferred --xres=1920 --yres=1080 --out=/tmp/gpu_probe.ppm --iters=1 --warmup=0

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ../build-gpu/GpuBench/GpuBench --fld=SCENES/GREETS.FLD --t=4871 --cam="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" --pass=deferred --xres=1920 --yres=1080 --out=/tmp/gpu_skipanim.ppm --iters=1 --warmup=0 --env_bake_skip_animated

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ../build-gpu/GpuBench/GpuBench --fld=SCENES/GREETS.FLD --t=4871 --cam="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" --pass=deferred --xres=1920 --yres=1080 --out=/tmp/gpu_envres512.ppm --iters=1 --warmup=0 --env_bake_skip_animated --env_res=512
```

Per-material decision dumps, both arms:

```
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ENVDBG=1 FDS_GREETS_CAM="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" FDS_GREETS_FOV=58.1092072 ./DEMO --snapshot=greets@t=4871 --out=/tmp/m_cpu_envdbg --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --dump_mats

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy FDS_ENVBAKE_DUMP=1 FDS_GREETS_CAM="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" FDS_GREETS_FOV=58.1092072 ./DEMO --snapshot=greets@t=4871 --out=/tmp/cv_envdump --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ../build-gpu/GpuBench/GpuBench --fld=SCENES/GREETS.FLD --t=4871 --cam="-4.11296749,3.19500089,-27.1061916,0.0976696312,-0.227209002,-0.968935847" --pass=deferred --xres=1920 --yres=1080 --out= --iters=1 --warmup=0 --dump_env_cube --dump_env_cube_dir=/tmp/gpuenv
```

### 11.7 One line on what a fix would be, since the explanation makes it obvious

Not applied, not recommended, stated because it follows directly: **E8 is the only one of the five
that is a data-quality question rather than an encoding one** — a normal map Sobelled out of camouflage
paint is not surface relief, and the gate that produces it is a substring match on the material name
(`strstr(n, "hull")`, `strstr(n, "cockpit")`, `DEMO/GREETS.CPP:1951-1953`). Removing `hull`/`cockpit`
from that name list is a one-line `DEMO/` edit that needs no flag and no authoring change; everything
else in §11.6 already has a flag (`--env_bake_linear`, `--env_bake_skip_animated`, `--env_res`,
`--deferred_checkerboard`) or is a deliberate design decision with its rationale in the source.

### 11.8 APPLIED 2026-08-09 — the gate is gone, E6/E7 have CPU flags, and P2 was a BUG

His three calls on §11.6, and what each one turned out to be. Every number here is MEASURED at the
same t=4871 pose as the rest of §11, on the 33,478-px canopy mask or the frame as stated.

**E8 — DONE, no flag.** `hull` and `cockpit` are out of the `strstr` list at `DEMO/GREETS.CPP:1951`.
Frame-wide **179,829 px (8.67 %)**, max channel Δ 164, 11,677 px > 10 luma; hull pixel (767,723)
**131.2 → 44.9** against the GPU's 41.0; canopy pixel (760,620) 146.4 → 157.4 against 161.4. The
greets byte pin moves `6780642b…` → **`9eeaf860cb5a7f124884a89e0fc3ff5b`** (3/3) — and note the pin
pose t=1588 changes by **one pixel at one LSB**, so the pin move is not the measurement.
Crop: `docs/img/mech/task1_mech_strip.png`.

`siling` is the ONLY material still reaching the Sobel bake. Its diffuse (`PSILING.JPG`, 256²) is a
diagonal-grained stone noise, so unlike camo paint its luminance is a defensible height proxy — but
the ceiling is almost always seen at a grazing angle, where a texel-frequency bump reads as sparkle.
Isolated (`--nmap_strength=0` now touches nothing else): **155,242 px (7.49 %)**, mean |ΔY| 9.24 on
changed, 54,184 px > 10 luma, concentrated in the top ~135 rows.
A/B at 3×: `docs/img/mech/task1_siling_zoom.png`. **The verdict is his**; if it stays, the authored
replacement is a `normal.png` in a new `Runtime/TEXTURES/PBR/<set>/` reached by an `RVSM` sub-chunk
on the `siling` surface in the LWO — `MaterialImport_ApplyRevMaps` then loads it with no code change,
and the name match can be deleted outright.

**E6 — `--env_bake_include_animated`** (CPU, default OFF, byte-null). Probe faces mean
**100.31 → 89.14**, all six 100 % nonvoid, −Y (toward the mech's own body) 96.22 → 74.55; canopy
**2,817 px**, mean |ΔY| 22.86 on changed, max 102.4; frame-wide 39,473 px (1.90 %). Mirror of the
GPU's `--env_bake_skip_animated` (5,268 px, mean 24.94). **Trap:** `g_envBakeSkipDynamic` is read in
THREE places in `Transform.cpp` and clearing it also disables the reflector's **own-face** skip —
the first cut did that and the +Y face came back 91 % VOID, probe mean 49.11. The flag hooks
`Transform.cpp:1274` only.

**E7 — `--env_mip_chain`** (CPU, default 0 = unset, byte-null). It reproduces the GPU's full-chain
select: at the shipped `--env_bake_res=256`, `--env_mip_chain=9` ≡ GPU `--env_res=256`; with
`--env_bake_res=128`, `--env_mip_chain=8` is an exact emulation of the GPU default. **But E7 is much
smaller on the CPU than §11 implied.** A within-arm sweep of the ISOLATED env term (render minus
`--no-env_refl`, 7×7 high-pass RMS over the canopy) gives **24.68 (default) → 24.32 (chain 9) → 24.05
(chain 8 + res 128) → 23.92 (chain 16, the bottom of the store's chain)** — **3 % across the whole
dial**, against the GPU's own `--env_res` sweep spanning 16.22 → 17.95 (11 %). §11's 9.97-vs-13.20
comparison was ACROSS arms, which differ in many things at once; the within-arm sweep is the cleaner
test. So on the CPU the canopy's high-frequency energy is **not** reflected detail — it is
Fresnel/normal modulation of an already-smooth reflection plus the frame ribs and the glass. What is
left between the two canopies is the env term's BRIGHTNESS (CPU +131.0 vs GPU +107.6 over the mask,
i.e. E0) and probe content.

**P2 — NOT a rate artefact. A BUG, and it is fixed.** See the dated note in `docs/SESSION_STATE.md`
and the `--deferred_checker_env_full` entry in `FeatureFlags.def`. Short form: the fill refuses to
average env-reflective pixels (`envForceFull`, `DeferredSurfaceKernel.cpp:5003`) and re-shades them
with a **reduced** kernel — no `--pbr` GGX lobe, no shadow terms at all, no AO, no nmap LOD fade —
so alternate pixels of every reflective surface carry two different BRDFs at a static
`(px ^ py) & 1` phase. Wave-2-minus-wave-1 mean luma on the canopy: shipped **+6.82**, `--no-shadows`
+5.51, `--no-pbr` **+0.90**, both off **−0.01**, full rate +0.04, GPU −0.05. Fixed → **+0.02**, and
**free** (`lighting-w2` 3.51 → 3.14 ms, 3/3 reps; the fill was already full-shading that set).
Crop: `docs/img/mech/task3_canopy_lattice.png`.

---

## 12. EVERY SHADING PATH AUDITED — the reduced-kernel sweep

**Why this section exists.** §11.8 root-caused one defect: the wave-2 checkerboard fill,
for **env-reflective** pixels, re-shades through a **REDUCED kernel** — a second
implementation of the same shading that had drifted from the primary. `--deferred_checker_env_full`
closed it (+6.82 → +0.02 luma odd-even bias, and it was cheaper). The obvious question was
whether that was the only one. **It was not.** This section is the systematic sweep: every path
that shades a surface, against the **scalar wave-1 deferred kernel**
(`Render_DeferredLighting_Tile`) as the reference.

> Everything below is **read out of the source** unless a row says MEASURED. All MEASURED numbers
> are runs made for this audit, dummy SDL drivers, at the poses named. Line numbers are anchors —
> the file moved under three concurrent agents while this was written; grep the symbol.
>
> **Measurement-environment caveat, stated up front.** Another agent held ~880 lines of
> uncommitted work in `FDS/RENDER/EnvBake.cpp`, `DEMO/GREETS.CPP`, `RENDER.CPP` and friends
> throughout this audit, so the greets and city **absolute** pins here (`778fa6ac…`, `3cbe42b1…`)
> are *not* the recorded `91ec081a…` / `5476be8c…` and that difference is **not mine**. Byte-null
> was therefore certified **differentially**: two binaries built back to back from the identical
> tree, one with my kernel diff and one with it reverted (`git checkout` + rebuild + `git apply`),
> with the non-kernel diff fingerprinted before and after. Fountain reproduces its recorded pin
> `8db68ccb59416e9a44037e9f387b7bd9` exactly, which is the one absolute anchor that survived.

### 12.1 The census — how many kernels shade a surface, and what each one has

Seven. The columns are the terms that actually separate them; **✓** = present and matching the
scalar reference, **✗** = absent, **≠** = present but computing something else.

| term | **scalar wave-1** (the reference) | wave-1 **vec** branch (`--deferred_vec`) | **OuterVec** (`Tile_OuterVec`) | **wave-2 fill** fallback (`TileFill`) | **transparent** (`…TransparentLighting_Tile`) | **forward** (`TheOtherBarry` + `Lighting()`) |
|---|---|---|---|---|---|---|
| SH ambient (`--sh_ambient`) | ✓ | ✓ (shared, outside the loop) | ✓ | ✓ | **✗** flat `Sc->Ambient` | **✗** flat `Sc->Ambient`, and **per-MESH** (`Faces[0].Txtr`), not per-pixel |
| emissive `Lum·255` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Lambert × linear falloff | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (per-VERTEX) |
| spot cone → **diffuse** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| spot cone → **specular** | **✗** (contract D4) | **≠ present** | ✗ | **≠ present** | ✗ | n/a |
| mirror-bounce window portal | ✓ | **✗** | **✗** | ✓ | ✗ | ✗ |
| 2-D spot map / mirror-clone shadow (`computeMapShadowAtten`) | ✓ | ✓ | **✗** | **✗** | **✗** | **✗** |
| cube tap + static shadow lightmap + PolyId (`resolveCubeAtten`) | ✓ | ✓ | **✗** | **✗** | **✗** | **✗** |
| `--shadow_bias` / `--shadow_slope_bias` | ✓ | ✓ | ✗ | ✗ | ✗ | ✗ |
| `--shadow_noncaster_depth` | ✓ | ✓ | ✗ | **✗** | ✗ | ✗ |
| `--pom_horizon` relief self-shadow | ✓ | **✗** | **✗** | **✗** | ✗ | ✗ |
| AO — `Mat_AoInAlpha` | ✓ | ✓ | **✗** (never fetches `texA`) | **✗** (never fetches `texA`) | `--xpar_pbr` only | ✗ |
| AO — `Mat->AoMap`, `--ao_direct` | ✓ | ✓ (forces scalar) | **✗** | **✗** | `--xpar_pbr` only | ✗ |
| normal map | ✓ (forces scalar) | — | ✓ | ✓ | `--xpar_pbr` | **✗** |
| normal-map **LOD fade** | ✓ | — | **✗** | **✗** | ✓ | ✗ |
| `--no_nmap` gate honoured | ✓ | — | **✗** | **✗** | ✓ | n/a |
| specular model | **GGX** under `--pbr`, else `pow_glossClass` | **GGX** (`run_vec_ggx_loop`) | **≠ Blinn-Phong `std::pow`** | **≠ Blinn-Phong `std::pow`** | **≠ Blinn-Phong `std::pow`** | **✗ NO SPECULAR AT ALL** |
| `--metal_spec_f0` (now **default 1**) | ✓ | **✗** (fixed F0 0.04) | **✗** | **✗** | ✗ | ✗ |
| metal diffuse kill on `fdB` (LDR) | ✓ | ✓ | **✗** | ✓ | ✗ | ✗ |
| metal spec tint | ✓ | ✓ | **✗** | ✓ | `--xpar_pbr` | ✗ |
| roughness map | ✓ | ✓ | ✓ | ✓ | `--xpar_pbr` | ✗ |
| env compose (`EnvSpecComposeScalar`) | ✓ | ✓ | ✓ | ✓ | **✗** | **≠** fixed 50 % `env + albedo>>1`, no Fresnel |
| `--diffuse_energy` `(1-F)` | ✓ | ✓ | ✓ | ✓ | — | — |
| `Material::SpecMul` | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ |
| `--texture_filter` / `--poly_viz` albedo plane | ✓ | ✓ | ✓ | **✗** | ✗ | ✗ |
| `--hdr_metal_kill` (default 2) | ✓ | ✓ | **✗** | **✗** | ✗ | ✗ |
| **writes `g_hdrBuf` at all** | ✓ linear | ✓ | **✗ — NOTHING** | ✓ | ✓ | ✗ (except the additive `HDRAccum` bolt) |

**The pattern the sweep confirms.** Every one of these is a *second implementation written for a
special case* (half-rate fill, 8-wide pixel batch, transparent layer) that then failed to inherit
terms added to the primary. Nobody removed a shadow tap from the fill; the shadow chain was
*added to wave 1* and the fill was never revisited. That is the same failure mode §8.4 named for
the `--hdr_linear` migration — *"the composite changed underneath expressions that nobody re-read"* —
one level up, at the kernel rather than the term.

### 12.2 FIXED — `--deferred_checker_edge_full`: the fill's OTHER reduced-kernel trigger

**The defect.** §11.8 / `--deferred_checker_env_full` fixed the fill's full-shade path for
**env-reflective** cells. But the fill has a **second** trigger for the same reduced kernel, and it
was left live: the averaging only runs when at least one neighbour is **compatible** (same matID,
`dot(N,Nₙ) ≥ --quarter_normal_cos`, relative `|ΔZ| ≤ --quarter_z_jump`). When **none** is — i.e. at
every **material / normal / depth EDGE**, which is every silhouette and every crease — it falls
through to the same reduced re-shade. Column 4 of §12.1 is the exhaustive list of what that
fallback omits. **Shadows matter most exactly at silhouettes**, and that is precisely the set that
was being shaded without any shadow term.

**The fix.** The fill's trigger reads **only** G-buffer + ZPage state, so wave 1 can evaluate it
exactly as the fill will. One shared function, `fillFallsBackHere()`, called from both waves —
wave 1 keeps the cell and shades it with the real kernel, the fill recognises the identical
condition (`matched == false` **is** the predicate, so the fill pays nothing for a second
evaluation) and leaves it alone. Env cells are excluded from this flag; they belong to
`--deferred_checker_env_full`.

**MEASURED.** The honest test is against the **full-rate ground truth**
(`--deferred_checkerboard=0`), over the mask of pixels the flag moves:

| pose | mask px | mean \|ΔY\| vs full-rate, OFF → ON | mean **SIGNED** ΔY vs full-rate, OFF → ON | max chan Δ | px > 10 luma |
|---|--:|---|---|--:|--:|
| greets `t=4871` (the §11 mech pose) | **85,606 (4.13 %)** | 0.947 → **0.584** | **+0.488 → −0.004** | 173 | 291 |
| greets `t=2000` (the projector, `--no-bloom`) | **164,393 (7.93 %)** | 0.409 → **0.144** | **+0.344 → −0.040** | 4 | 0 |
| greets `t=1588` (the pin pose, `--no-env_refl`) | 5,252 (0.25 %) | 1.144 → **0.785** | **+0.569 → −0.153** | 118 | 52 |

Same signature as the env row: a **one-sided bias** (the reduced kernel is systematically
*brighter*, because it drops every shadow term) closed to ~0. The residual \|ΔY\| that remains is
the fill's *reconstruction* of the other cells, which is a rate question, not a kernel question.

**COST — and unlike the env row this one is NOT free.** `--bench=scene@scene=greets,t=4871,iters=40`,
`--deferred_prof=5`: `lighting-w1` 32.1 → 34.5 ms and `lighting-w2` 3.48 → 4.62 ms on the first
rep; a second rep landed 46.1 → 41.3 and 7.25 → 5.08, i.e. **inside the run-to-run spread of this
harness on a 12-worker pool.** What is *not* noise is the mechanism: 85,606 px move from the cheap
fallback to the full kernel, and at wave-1's measured ~0.36 µs/px that is ≈31 ms of thread-sum,
≈2.6 ms of wall at the observed 11.2× parallel efficiency. **Treat the price as ≈2–3 ms/frame at
1920×1080 on greets, not as zero.** (The first cut cost ~4 ms because it evaluated the predicate
twice; folding the fill's side onto `matched` removed that half.)

**Default OFF.** It is a real fidelity fix, it is a **look** change on 4–8 % of the frame, and it
costs single-digit milliseconds. That trade is the user's call, not mine.

### 12.3 FIXED (no flag, inert at defaults) — OuterVec dropped cells the fill refused to fill

**A hole class, not a shading class.** `Render_DeferredLighting_Tile_OuterVec` implements the
checkerboard/quarter wave-1 drop on **parity alone**. It has no `--deferred_checker_env_full`
branch. But the fill honours that flag (**default 1** since `1782351`) and *skips* env-reflective
dropped cells, expecting wave 1 to have taken them. Under `--deferred_outer_vec
--deferred_checkerboard` those cells were therefore shaded by **neither wave**.

MEASURED, greets `t=4871`, LDR (`--no-hdr`), against that configuration's own full-rate render:

| | changed px | mean \|ΔY\| on changed | max chan Δ | px > 10 luma | whole-frame ΔY |
|---|--:|--:|--:|--:|--:|
| **before** | 368,684 (17.78 %) | **3.958** | **255** | 17,887 | −0.566 |
| **after** | 354,481 (17.09 %) | **0.815** | 61 | 1,739 | **−0.001** |

`max chan Δ = 255` is the signature: literal unshaded holes. The scalar control
(scalar wave-1 + checkerboard vs scalar full-rate) sits at mean 1.434 / max 167 at the same pose,
so 0.815 is now *better* than the reference path's own reconstruction error.

Fixed in place with no flag: the OuterVec drop mask re-admits an env-qualified lane, and the fill's
`--deferred_checker_edge_full` skip is additionally gated on `!deferredLightingOuterVecEnabled()`
(OuterVec cannot evaluate the edge predicate either, so the new flag must not create the same hole
class). Both are unreachable at the shipped defaults — **no scene sets both** (greets = checkerboard
with `PreferOuterVec = 0`; city/fountain/crash = OuterVec with no checkerboard) — hence byte-null by
construction and no flag.

**REPORTED, NOT FIXED, and it is bigger:** under **`--hdr`** the same combination is broken for a
different reason and remains so. OuterVec writes **nothing** to `g_hdrBuf`; the fill's HDR path
averages the *neighbours'* `g_hdrBuf` and then stamps `h[3] = 1.0` (covered). So every dropped cell
becomes "covered, zero radiance" and `Hdr_ActivateNoFog` can no longer lift it from VPage.
MEASURED at greets `t=4871`: `--deferred_outer_vec --deferred_checkerboard` vs the same with
`--deferred_checkerboard=0` is **99.94 % of pixels, whole-frame Y 57.6 vs 113.9**. The honest fix is
either an HDR write in OuterVec or a hard refusal of the combination under `--hdr`; both are larger
than this audit and neither is reachable by default.

### 12.4 THE SIMD VERDICT — `--deferred_vec`, asked for by name

**The flag's own documentation is STALE, in two places.** `FeatureFlags.def:32` and
`FeatureFlags.h:24-26` both say *"still omits 2D spot-shadow maps + mirror-clone shadows."* Read the
code: the vec lane loop calls **`computeMapShadowAtten(tl, gi, ctx, …)`**, and that function's own
contract comment says it covers *"mirror-clone source maps (srcSm), mirror-clone source cubes
(srcCube), and the light's own 2-D spot map (smIdx + PCF)"* — i.e. exactly the two things the caveat
claims are missing. It also calls `resolveCubeAtten` per lane and folds the result into
`coneShadowAtten` so the templated spec loop applies it. **That caveat describes a state the code
left behind.**

**MEASURED (greets `t=4871`, arm64, vec vs scalar, same binary):**

| arm | changed px | mean \|ΔY\| on changed | max chan Δ | px > 10 luma |
|---|--:|--:|--:|--:|
| `--deferred_vec` (shipped `--pbr`) | 32,967 (1.59 %) | **7.931** | **164** | **4,643** |
| `--deferred_vec --deferred_vec_force` | 148,246 (7.15 %) | 2.079 | 164 | 4,663 |
| `--deferred_vec --prof_no_spec` (**diffuse only**) | 16,350 (0.79 %) | **0.377** | **6** | **0** |
| `--deferred_vec --no-pbr` (**Blinn-Phong both sides**) | 63,739 (3.07 %) | 1.005 | **12** | **0** |
| `--deferred_vec --no-shadows` | 66,138 (3.19 %) | 4.996 | 168 | 6,438 |
| `--deferred_vec --no-pom_horizon` | 32,967 | 7.931 | 164 | 4,643 (**identical to shipped**) |

Read the ablations in order and the answer is forced:

1. **The diffuse term AGREES.** With specular removed, the two kernels differ by max **6/255** and
   **0 px** above 10 luma. That is float noise, and it is also the measurement that retires the
   stale shadow caveat — a vec path missing the 2-D spot map or the mirror-clone shadow could not
   agree with the scalar path to 6/255 on the diffuse term.
2. **It is not the shadows** — the gap *survives* `--no-shadows`.
3. **It is not `--pom_horizon`** — that arm is bit-identical to the shipped one.
4. **It is the GGX lobe, and only under `--pbr`.** Under Blinn-Phong the same vec path lands at max
   12/255 with 0 px over 10 luma.

**Mechanism, isolated by a roughness sweep** (`--pbr_roughness` forces the lobe width on both arms):

| `--pbr_roughness` | changed px | mean \|ΔY\| | max chan Δ | px > 10 luma |
|---|--:|--:|--:|--:|
| **0.2** (≈ the shipped gloss-48/64 lobes) | 33,474 | **8.379** | **178** | **5,091** |
| 0.5 | 25,708 | 0.421 | 6 | 0 |
| 1.0 | 17,005 | 0.374 | 6 | **0** |

At `rough = 1` the GGX denominator `NdotH²(a²−1)+1` is **exactly 1**, D is constant, and the
divergence collapses to the same 0.37 / max-6 floor as the diffuse-only arm. At `rough = 0.2` that
denominator is `1 − 0.9984·NdotH²`, which is ≈0.0016 at the highlight core, and `D ∝ 1/d²` — so the
lobe **amplifies small input errors by orders of magnitude at exactly the pixels that are bright**.

The only arithmetic difference between the two GGX implementations is precision, read from source:
`run_vec_ggx_loop` takes `_mm256_rsqrt_ps` for both `lenInv` and `hInv` and `_mm256_rcp_ps` for the
D denominator, `Gv`, `Gl` and `1/(4·NdotV)` — **five approximate reciprocals with no
Newton-Raphson** — where the scalar body uses exact divides and the NR-corrected `fast_rsqrt`. On
x86 those are the native 12-bit `rsqrtps`/`rcpps`; on arm64 simde lowers them to bare
`vrsqrteq_f32` / `vrecpeq_f32` (`FDS/simde/x86/sse.h`), an ~8-bit estimate — **coarser than x86, not
finer.** (The lobe-width sweep is MEASURED; the attribution to these five instructions is
READ-FROM-SOURCE by elimination, since nothing else in the two GGX bodies differs at
`metalness = 0`.)

**Structural divergences the vec path also carries** (source, not measured — each needs a pose I
could not make fire at t=4871): the **mirror-bounce window portal** (`bouncePortalReject`) is absent
from the vec diffuse loop; **`--pom_horizon`** is absent; **`--metal_spec_f0`** is absent from
`run_vec_ggx_loop` (fixed F0 0.04) and that flag is now **default 1**, so on any conductor the vec
path's direct lobe is a different quantity — the mech has no metalness, which is why it does not
show at this pose.

**VERDICT.** *Can the two be reconciled?* **The diffuse halves already are** — to 6/255, including
every shadow term. **The specular halves cannot be, as written**: the vec GGX is not a different
model, it is the *same* model evaluated through five un-refined reciprocal estimates, and a
`rough ≈ 0.2` lobe has no tolerance for that. Reconciling means one NR step on each (the OuterVec
normal decode already carries exactly this fix, with a comment naming the identical failure —
"the raw 12-bit rsqrt … snapped the reflected direction ~0.35° per LUT-cell crossing"), which
gives back most of what the approximations bought. **So the x86 default is shipping a different
image from arm64's** — 4,643 px more than 10 luma apart on one greets frame — and the flag doc
does not say so; it warns about shadows, which are fine. **Not fixed here**: I cannot measure the
x86 arm, and adding NR to the vec lobe is a perf decision on the platform where the path is ON by
default.

### 12.5 The QUARTER path

`--deferred_quarter` has **no kernel of its own**. It reuses `Render_DeferredLighting_Tile` for
wave 1 (same drop test, `(px|py)&1`) and `Render_DeferredLighting_TileFill` for wave 2 (different
neighbour *pattern* — 2 horizontal, 2 vertical or 4 diagonal — into the *same* averaging and the
*same* reduced fallback). So it inherits §12.2 exactly, over a **3× larger** dropped set.
`--deferred_checker_edge_full` covers it: `fillFallsBackHere()` implements both neighbour patterns.
Not measured — no scene ships `--deferred_quarter`.

### 12.6 The FORWARD path — deliberate, but the contract described it WRONG

§0 and §2 of this document say the forward/transparent arm runs "Blinn-Phong". **For the forward
filler that is false.** `grep -c "NdotH\|Specular\|Glossiness" FDS/RENDER/Lighting.cpp
FDS/FILLERS/TheOtherBarry.h` returns **0 and 0** (MEASURED). The forward path has **no specular
term of any kind** — `Material::Specular`, `Glossiness` and `SpecMul` are never read on it, and
`The_Specular_Gouraud` / `The_Specular_TGouraud` in `FILLERS.CPP` are inside a comment block and
unreferenced. What it has: flat `Sc->Ambient` **per MESH** (from `Faces[0].Txtr`, not per pixel),
per-vertex Lambert × linear falloff, the spot cone, per-vertex `sqrt` fog, a fixed **50 %**
`env + albedo>>1` blend for `Face_Reflective` with **no Fresnel and no roughness lobe**, and no
shadow / AO / normal / roughness / metal map at all. It writes 8-bit VPage and stamps the
`0xFFFFFFFF` sentinel so the deferred kernel skips those pixels.

**DELIBERATE — do not "fix".** It is the 1998 vertex model, it survives only for
`Face_Reflective` and `Mat_Additive` faces plus the RTT/shard forward fallbacks, and the GPU arm
does not implement its layers at all. **Corrected in the record**, not changed in the code. One
genuine unit wrinkle worth keeping: those VPage pixels are later lifted into `g_hdrBuf` and
**squared** by `Hdr_ActivateNoFog` even though `texel·light/256` was never gamma-encoded — the
same mixed-unit class as E0.

The **transparent** kernel is a separate story and is closer to the reference: it has the normal
map, the LOD fade, `SpecMul`, the `--hdr_linear` write and (under `--xpar_pbr`, default 0) AO,
roughness and metal maps. What it lacks: SH ambient (flat `Sc->Ambient`), **any** shadow tap — note
`surfaceShadowId` is computed and then **never read**, a dead local — the GGX lobe, and the env
compose. Recorded as SCOPE per §0; unchanged.

### 12.7 The OFFSCREEN passes

| pass | kernel it runs | what it gets |
|---|---|---|
| **shadow cube / 2-D bake** (`Shadows.cpp`) | `MekaleleShadowDepth` | **Depth + polyId only, no colour.** Not a shading path. Recorded so nobody audits it again. |
| **env probe bake** (`EnvBake.cpp renderSixFaces`) | the **full `renderFrame`** at face res, `RenderPath::ForceDeferred`, `skipVolumetric=true` | so it gets whichever wave-1 kernel the scene's policy selects — **including OuterVec on city/fountain/crash**, i.e. probes baked with no shadows and no GGX. Capture is a **deliberate mix**: linear `g_hdrBuf` where `h[3] > 0`, the 8-bit LDR VPage everywhere else (sky, void, forward-filler content). E0/§9.3 own the encoding half. |
| **greets mirror RTT** (`GreetsMirror.cpp`) | `Render_DeferredLighting` with `inlineDispatch`, at RTT res | **it DOES get the HDR-linear composite** — `Hdr_BeginFramePass(texW, texH)` is called first, so `Hdr_WritableFor` passes. **Two comments say otherwise and are stale**: `Hdr.h:44-51` and the `hdrWrite` comment in `DeferredSurfaceKernel.cpp` both assert the RTT "never ran `Hdr_BeginFrame`, so `g_hdrBuf` is unsized there". **REPORTED, NOT FIXED:** only order-1 slots that carry a `textTex` publish the pre-tonemap float radiance (`Mat->hdrRefl`); every other slot — order-2, recursive, `--no-hdr`, `--no-shard_deferred` — has ACES + the sqrt encode applied at RTT res and then **again** at the frame tonemap. The reflection is **tonemapped twice**. |
| **mirror-shatter shard bake** (`MirrorShatter.cpp`) | `Render_DeferredLighting` with `inlineDispatch`, at `greets_shard_res = 64` | **LDR gamma composite** — there is no `Hdr_*` call anywhere in that file, so `Hdr_WritableFor(64,64)` is false and the 250 cap is live. The 8-bit atlas is then used as an ordinary albedo texture, which the main kernel **squares** under `--hdr_linear`. That is exactly E0's "a gamma value entering the linear composite", on the shard atlas, and **it has no flag** the way `--env_bake_linear` does. REPORTED, NOT FIXED — unpriced, and `MirrorShatter.cpp` was not this audit's file to change. |

### 12.8 What this section changes, and what it leaves owed

**Fixed:** `--deferred_checker_edge_full` (default OFF, byte-null, numbers in §12.2); the
OuterVec/checkerboard hole class (no flag, unreachable at defaults, numbers in §12.3).

**Byte-null certified** differentially against a binary built from the identical tree with the
kernel diff reverted: greets `778fa6ac…`, fountain `8db68ccb…` (= its recorded pin), city
`3cbe42b1…` — all three identical on both arms. `tools/render_gate.sh` **3/3 PASS**.
`make wasm` builds.

**Owed, in the order I would take them:**

1. **The vec GGX lobe** (§12.4) — one Newton-Raphson step on each of the five reciprocals, measured
   on x86 where the path is default ON. This is the one that ships a different image to different
   users.
2. **OuterVec has no HDR write** — city, fountain and crash all run it by scene policy
   (`Scene::PreferOuterVec = 1`, `CITY.CPP`, `FOUNTAIN.CPP`, `CRASH.CPP`) and it is the *only* kernel
   with no `g_hdrBuf` store at all, so their whole `--hdr` frame is the clamped 8-bit LDR combine
   lifted by `Hdr_ActivateNoFog`. Priced here for the first time: on **fountain t=2500**, OuterVec
   against the scalar wave-1 kernel is **285,868 px (13.79 %), mean \|ΔY\| 3.774, max chan Δ 205,
   20,229 px > 10 luma** (MEASURED, `--no-deferred_outer_vec` A/B). On **city t=1961** the same A/B
   is **91,270 px, mean 0.429, max 3, 0 px > 10 luma** — inert, because city's materials are matte
   and it has no live shadow/AO/metal term for OuterVec to be missing. So this is a **fountain**
   problem, not a city one.
3. **The mirror RTT double tonemap** and the **shard atlas gamma-into-linear** (§12.7).
4. **`--metal_spec_f0` is default 1 and three of the seven kernels do not implement it** — the vec
   GGX, OuterVec and the wave-2 fill. Inert wherever there is no metalness map; live on `momy`,
   `amudim`, `screen emiter`.
5. **The two stale doc strings** — the `deferred_vec` shadow caveat (`FeatureFlags.def:32`,
   `FeatureFlags.h:24-26`) and the mirror-RTT "g_hdrBuf is unsized there" comments (`Hdr.h:44-51`
   and the kernel). Both actively mislead; neither file was mine to edit under the contention.

---

## 13. AO, the SECOND implementation — GTAO ported into the GPU arm

Commissioned verbatim: *"make the gpu test have gtao"*. Until now the GPU arm had **no ambient
occlusion at all**, so contract rows O1–O5 / D6 / D6b covered only the *map-based* AO in the
G-buffer alpha; the CPU's screen-space `Render_SSAO` (`FDS/RENDER/DeferredSSAO.cpp`) had no
second implementation to be checked against. It has one now.

**What was written.** `fs_ssao` / `fs_ssao_blur` / `fs_ssao_apply` in
`GpuBench/shaders/deferred.metal` — a port of the CPU pass with the CPU as the authority on every
expression: the same GTAO + 32-sector visibility bitmask (Therrien & Levesque 2023), the same
Eberly `acos` fit, the same 5-term minimax `atan2` (`FDS/FILLERS/SimdHelpers.h:36-77`, **not** MSL's
`atan2` — that would be a different function, not a better one), the same 16-entry 4×4 tiling
rotation, the same per-frame **slice-azimuth table** (`buildSliceTrig`, c78d536f: `phi = (s+jit)·π/slices`
takes only `slices×16` values a frame — the host fills the table with the identical expression so
the float that reached `cosf` on the CPU is the float the shader reads), the same matched 4×4
depth-only denoise, the same 4-tap depth-aware bilinear upsample, and the same apply point:
multiplying the **linear radiance** straight after the lighting pass and before
flares / cones / transparents / bloom / tonemap, main view only — the CPU's `!skipVolumetric` gate
at `RENDER.CPP:729-730`. The hemisphere fallback (`--ssao` without `--ssao_gtao`) is ported too.

**Not ported, stated so nobody reads more into the numbers:** `--ssao_temporal` (CPU default 0),
and the CPU's 8-wide tail-overlap trick (a GPU has no SIMD remainder).

### 13.1 The CLI — one command line, both renderers

`GpuBenchMain.mm` mirrors the FDS flag **names and defaults**, and normalises `-`→`_` exactly as
`FeatureFlags.cpp:284` does, so `--ssao --ssao-gtao --ssao-downscale=2` means the same thing to
`./DEMO` and to `./GpuBench`: `--ssao`, `--ssao_gtao`, `--ssao_downscale`, `--ssao_samples`,
`--ssao_radius`, `--ssao_strength`, `--ssao_bias`, `--ssao_power`, `--ssao_blur`,
`--ssao_gtao_slices`, `--ssao_gtao_steps`, `--ssao_gtao_thickness`. Three are new instruments:
`--ssao_dump=PATH`, `--ssao_ref=PATH`, `--slow_math` (§13.3).

### 13.2 The comparable quantity is the AO FIELD, not the lit frame

A lit-frame diff cannot price AO here: D1, D3, D6, D7, E6, E7 and E8 all move the same pixels. So
both arms now emit the AO **multiplier** — the exact scalar the apply multiplies radiance by,
after denoise and upsample — plus its inputs, in one shared file:

```
AOF3 = "AOF3" + int32 w + int32 h + w*h f32 AO + w*h f32 view-Z (<0 = sky) + 3*w*h f32 geometric normal
```

CPU side: `--ssao_dump` (new **default-off** flag, `FeatureFlags.def`, path from
`FDS_SSAO_DUMP_PATH`, noinline reporter). Instrument state is declared in the flag help: it forces
the generic scalar apply loop instead of the aarch64 `vld4/vst4` fast path (bit-identical by
construction — `x*1.0f` and the f16 round trip are the same round-to-nearest) and allocates one
`XRes*YRes` float plane. It does **not** touch the compute or the denoise.

**BYTE-NULL, twice over.** ABSOLUTE: the recorded greets acceptance pin reproduces exactly with
this diff in — `./DEMO --snapshot=greets@t=5743 --out=<dir> --deferred --hdr --hdr-linear
--texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0` gives
**`26ad272aaa6cc9050c66e84cdaaf5436`**, the value in the gates table. DIFFERENTIAL, taken first
(before this tree's assets were confirmed to match HEAD): two DEMO binaries built back to back from
the identical tree, one with the FDS diff and one with it reverted, greets `t=5743`
`ff2169dfa37317081b60b3d63d0aba49` / `t=2845` `7e0dba9dfca9149992e0ef1cf5f25f83` — identical on
both. (Those two are the same recipe **without** `--greets-displace`, which is why they are not the
pin.) On the GPU side, with `--ssao` off
the frame is md5 **`d3a8301a22495c80dfdd5c3f8509f771`** at the §11 pose — the value already recorded
in E6 — even though the normal plane widened from `RG16Snorm` to `RGBA16Snorm`.

**Why the normal plane widened.** The CPU G-buffer carries exactly ONE normal and it is the
**geometric** one (`Mekalele.h:3294` encodes the interpolated vertex normal; the normal map is
applied *inside* the lighting kernel at `DeferredSurfaceKernel.cpp:1720-1727`), so `Render_SSAO`
reads geometric normals. This arm bakes the **perturbed** normal — right for lighting, wrong for a
port of the AO, because GTAO's horizon reference angle is a first-order function of it. `.xy` still
holds the shading normal bit-for-bit; `.zw` now holds the geometric one and the AO reads that.

### 13.3 The result, and where the residual lives

Four greets probe poses, 1920×1080, `--ssao --ssao-gtao` at the shared defaults
(slices 2, steps 4, radius 4, strength 1.5, thickness 1, blur 2, downscale 1). Contract line, per
pixel: `pass ⇔ |gpu − cpu| ≤ 0.005·max(|cpu|,|gpu|) + 1e-4`.

| pose | arm | AO mean CPU → GPU | mean \|Δ\| | max \|Δ\| | **PASS-RATE** |
|---|---|---|--:|--:|--:|
| t=4871 | as shipped (each arm's own G-buffer) | 0.85431 → 0.85639 | 0.00653 | 0.5077 | 51.93 % |
| | **same inputs**, CPU = shipped 8-wide | 0.85431 → 0.85597 | 0.00259 | 0.0234 | 75.79 % |
| | **same inputs**, CPU = its own SCALAR reference | 0.85601 → 0.85597 | **0.00009** | 0.0059 | **99.979 %** |
| t=5743 | as shipped | 0.82507 → 0.81199 | 0.01753 | 0.9433 | 31.10 % |
| | same inputs, 8-wide | 0.82507 → 0.82553 | 0.00135 | 0.0234 | 88.15 % |
| | **same inputs, scalar ref** | 0.82556 → 0.82553 | **0.00007** | 0.0073 | **99.931 %** |
| t=2845 | as shipped | 0.94410 → 0.94989 | 0.00587 | 0.0293 | 49.41 % |
| | same inputs, 8-wide | 0.94410 → 0.94468 | 0.00093 | 0.0147 | 98.01 % |
| | **same inputs, scalar ref** | 0.94468 → 0.94468 | **0.00000** | 0.0029 | **100.000 %** |
| t=6097 | as shipped | 0.91863 → 0.93551 | 0.01834 | 0.3355 | 19.09 % |
| | same inputs, 8-wide | 0.91863 → 0.91870 | 0.00058 | 0.0088 | 99.83 % |
| | **same inputs, scalar ref** | 0.91870 → 0.91870 | **0.00000** | 0.0020 | **100.000 %** |

Read the three rows of each pose in order and the localisation is forced.

**Row 3 is the verdict on the PORT: it is the same function.** Two of four poses pass the contract
on **every one of 2 073 600 pixels**; the other two pass on 99.93 % / 99.98 %, mean \|Δ\| 7–9e-5,
and every failing pixel is off by **at most one sector bit out of the 64** a 2-slice 32-sector
bitmask can set.

**Row 1 minus row 2 is the G-BUFFER, not the AO.** `--ssao_ref` (new, §13.2) drives the GPU AO from
the CPU dump's own depth and geometric-normal planes. Turning it on at t=6097 moves the pass rate
19.09 → 99.83 %. The inputs really are different: at t=5743 the two arms' depth planes differ by
rel \|ΔZ\| mean 0.0047 (max 10.07 at a silhouette) and their geometric normals by 2.55° mean. That
is two rasterisers, the CPU's 16-bit `ZPage16` against `Depth32Float`, and A1's mip/coverage
question — all pre-existing contract rows, now measured through a new instrument.

**Row 2 minus row 3 is a CPU-INTERNAL divergence, and it is the biggest single term.** The CPU's
8-wide GTAO (`DeferredSSAO.cpp:358-489`) uses `_mm256_rsqrt_ps` with **no Newton-Raphson** for the
two horizon cosines `dinv`/`binv`; on arm64 simde lowers that to bare `vrsqrteq_f32`, an ~8-bit
estimate. The scalar reference right above it uses `fast_rsqrt`, which has the NR step. Priced by
differencing the CPU against **itself** (`FDS_SSAO_NOSIMD=1`, the file's own escape hatch):

| pose | CPU 8-wide vs CPU scalar — pass-rate | CPU 8-wide vs GPU (same inputs) — pass-rate |
|---|--:|--:|
| t=4871 | 75.85 % | 75.79 % |
| t=5743 | 88.29 % | 88.15 % |
| t=2845 | 98.00 % | 98.01 % |
| t=6097 | 99.83 % | 99.83 % |

The two columns agree to a tenth of a percent at every pose: **the whole same-inputs CPU-vs-GPU gap
IS the CPU's own SIMD-vs-scalar gap.** Direction and size: the shipped 8-wide path is systematically
**more occluded** than its own scalar reference — AO mean 0.85431 vs 0.85601 at t=4871 — i.e. 0.07
of one sector bit per pixel, darker. With the denoise off so each cell is a raw bitmask, the
difference is **exactly** integral: 82.60 % of pixels bit-identical, 4.94 % off by −1 bit, 11.79 %
by +1, 0.65 % by ±2, and **0.0037 % not an integer number of bits**. A hard-quantised integrator
has no tolerance for an 8-bit reciprocal.

**This is §12.4's `--deferred_vec` finding, in a second kernel.** Same mechanism (un-refined
`_mm256_rsqrt_ps`/`_mm256_rcp_ps` under simde on arm64), same consequence (x86's 12-bit estimate and
arm64's 8-bit one ship different images), now on the AO pass, and here it is measurable against the
CPU's own reference with one environment variable.

**Ruled out, measured, so it is not re-litigated:** MSL fast math. `--slow_math` (new, compiles the
library with `fastMathEnabled = NO`) moves the t=4871 blur-off arithmetic-only comparison from
360 790 failing pixels to 360 793 — **three pixels**. Metal's approximate divide/sqrt and free FMA
contraction are not the source.

**Hemisphere fallback** (`--ssao` without `--ssao_gtao`), t=4871: as shipped 77.14 %, same inputs
**98.22 %**, mean \|Δ\| 0.00013. Its residual has the same cause and **no escape hatch** — the CPU's
8-wide hemisphere path uses raw `_mm256_rcp_ps` twice and `FDS_SSAO_NOSIMD` only guards the GTAO
branch.

**`--ssao_downscale=2`** (the value a concurrent round is making the default) reproduces the whole
picture, so the reduced-res compute + upsample is faithful too: t=4871 scalar-ref **99.973 %**,
t=5743 **99.925 %**.

### 13.4 Cost, on the GPU arm

greets t=5743, 1920×1080, median of 120 frames after 50 warmup, per-encoder timestamps:

| arm | `ssao` pass | FRAME TOTAL |
|---|--:|--:|
| no AO | — | 4.83 ms |
| `--ssao --ssao-gtao` (downscale 1) | 1.94 ms | 5.79 ms |
| `--ssao --ssao-gtao --ssao-downscale=2` | 1.57 ms | 5.29 ms |
| `--ssao` (hemisphere, 16 samples) | 2.04 ms | 6.32 ms |

### 13.5 Images

`docs/img/gpugtao/` — per pose `greets_t<T>_lit.png` (CPU | GPU × AO off / AO on) and
`greets_t<T>_aofield.png` (CPU AO field | GPU AO field | \|Δ\|×10), for t = 4871, 5743, 2845, 6097;
plus `greets_t4871_localise.png` and `greets_t5743_localise.png`, which put \|Δ\|×10 against the
shipped 8-wide CPU next to \|Δ\|×10 against the CPU's own scalar reference — the right panel is
black.

### 13.6 What this owes

1. **The CPU's 8-wide GTAO reciprocals** — one NR step on `dinv`/`binv` in `gtaoRow8`, and on the
   two `_mm256_rcp_ps` of the hemisphere path. It is the same item §12.8 already ranked first for
   the vec GGX lobe, and this section prices it on a second kernel: up to 24 % of pixels differ from
   the file's own scalar reference, systematically darker. **Reported, not fixed** —
   `FDS/RENDER/DeferredSSAO.cpp` is a contended file and this is a perf decision on a path that is
   ~40 % of his acceptance frame.
2. **`--ssao_temporal` has no GPU arm.** The flag is accepted and warns.
3. **Contract rows O1–O5 still describe only the map-based AO.** The screen-space AO now has a
   second implementation; a proper row set for it belongs in §2.8 next time that table is touched.

### 13.7 Reproducing it

`tools/ssao_gtao_battery.sh <tag> <t> <cam>` renders all six arms of one pose (CPU/GPU × AO off/on,
plus the two `--ssao_ref` localisation arms) into `/tmp/gtao/<tag>/`;
`tools/ssao_ao_compare.py <cpu.f32> <gpu.f32> [label]` prints the table above for a pair. Both live
in `tools/` rather than in an untracked battery script, because §"the greets acceptance pins were
never orphaned" is what happens when they do not. The four poses used:

```
t=4871  -4.11296749,3.19500089,-27.1061916,0.0976696834,-0.227209285,-0.968935847
t=5743   9.07557869,3.19592357,-52.9277191,-0.206726,-0.140846923,0.968207955
t=2845  -7.38721609,2.72471762,-50.8239441,0.817980409,-0.113631025,0.563911498
t=6097  18.4499683,5.16043377,-57.6482239,-0.824409068,-0.544821978,-0.153358385
```

all at `FDS_GREETS_FOV=58.1092072`, 1920×1080. `GpuBench` prints the paired `./DEMO` command
(including the FOV) in its own `[POSE]` block, so the pair never has to be re-derived.
