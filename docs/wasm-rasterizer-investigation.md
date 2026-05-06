# Wasm rasterizer investigation — lab notes

Personal-fresh notes from chasing a 4-5× wasm-vs-native gap in the FDS
rasterizer. Captures observations, code snippets, dead ends, and the fix.
Polished writeup deferred — this is the raw material.

## Setup

- 1998 demoscene project (FLOOD/REVIVAL) being revived; software rasterizer
  in `FDS/FILLERS/TheOtherBarry.h` (tile-based AVX2 SIMD path).
- Native: arm64 macOS via simde mapping `_mm*` → NEON.
- Wasm: emcc → wasm32, simde mapping `_mm*` → wasm SIMD128.
- Vendored: simde 0.7.3 (now 0.8.2 + patches) and vectorclass 2.01.03.

Initial bench: synthetic full-screen quad (FillerTest fixture), 1920×1080,
500 iters native / 200 iters wasm.

| | native ms/iter | wasm ms/iter | ratio |
|---|---|---|---|
| full | 1.154 | 4.597 | 4.0× |
| city@t=1961 | 16.286 | 28.118 | 1.7× (saved 12 ms/frame) |
| greets@t=600 | 3.480 | 9.275 | 2.7× |

Goal: figure out what's costing 12 ms/frame on city@wasm.

## Methodology that worked: op-skip variant deltas

Compile the rasterizer with one inner-loop op stubbed out, time it, attribute
the delta to that op. Variants live in `FDS/FILLERS/TheOtherBarry.h` behind
`#if BENCH_SKIP_TEXTURE` etc., selected by `-DBENCH_VARIANT=no-texture` (etc.)
in CMake. Stubs replace the op with a constant or a pass-through; visual
output is wrong but the timing is meaningful.

Variants:
- `full` — production rasterizer (baseline)
- `no-texture` — stub the gather: `auto texture0_samples = Vec8ui(0xFFFFFFFF);`
- `no-perspective` — drop `* p_z` from the UV mul
- `no-maskstore` — replace masked write with unmasked
- `no-z` — skip Z compute / Z-test / Z-write entirely (still gate the inner
  block on `any_lane_set(p_mask)` so the texture path still runs)
- `no-color` — skip the colorize() per-lane gouraud blend, pass texture sample
  straight to the maskstore

Run each variant on each workload on each target → 36-cell matrix.

Per-op cost = `time(full) − time(no-X)`. Wasm tax per op = `wasm_cost − native_cost`.

### Key bench tables (before any fix)

Native per-op cost on synthetic-quad: texture 0.41ms (36%), z 0.18, color 0.21,
perspective 0.07, maskstore 0.09. Texture dominates as expected.

Wasm tax per op (wasm − native), city@t=1961:

| op | tax ms | comment |
|---|---|---|
| texture | +1.28 | gather, expected |
| color | **+12.49** | smoking gun |
| z | −7.06 | misleading: stubbing z lets more pixels through to colorize |
| perspective | −0.51 | within noise |
| maskstore | +0.05 | within noise |

color tax is 88% of total. Initial conclusion: "colorize is the wasm bottleneck."
First-instinct fix: rewrite colorize() with a template-dispatched struct
(native via vectorclass, wasm via direct `wasm_simd128.h` intrinsics).

**That conclusion was wrong about the cause and about the fix.**

## What colorize actually is

`SimdHelpers.h:115`:
```cpp
template <uint8_t Shift = 0>
inline Vec32uc colorize(Vec32uc color1, Vec32us color2) {
    return compress((extend(color1) * (color2 >> Shift)) >> 8);
}
```

8 RGBA pixels × 4 bytes = 32 u8 (color1, texture sample). 32 u16 (color2,
gouraud-interp blend). Extend u8→u16, multiply, shift, narrow back.

Lighting clamps gouraud to ≤250 (`RENDER.CPP:1352-1356`), so color2 fits in
8 bits — relevant for a planned but ultimately unused optimization.

## The disasm step

Bench tool: `make bench-scenes-wasm BENCH_ITERS=200`. Build artifact at
`build-wasm-bench/full/DEMO/DEMO_snapshot.wasm`. Disassemble:

```sh
WASM_DIS=$(brew --prefix)/Cellar/emscripten/*/libexec/binaryen/bin/wasm-dis
$WASM_DIS DEMO_snapshot.wasm > out.wat
```

Symbols are stripped from the bench artifact. Build a separate variant with
`-g2` to get the C++ name section (mangled but searchable):

```sh
emcmake cmake -DCMAKE_CXX_FLAGS=-g2 -DCMAKE_EXE_LINKER_FLAGS=-g2 ...
```

Find the rasterizer:

```sh
grep -nE "TheOtherBarry<.*TBlendMode.291.*TTextureMode.290" out.wat
```

That gets the most-used template instantiation (BlendMode=OVERWRITE,
TextureMode=NORMAL). Body is ~3500 lines of wat.

### What the disasm showed

Inside apply_exact's hot loop, four 16-iteration **scalar** mul loops:

```wat
(loop $label3
  (v128.store offset=128 ($71))   ;; spill SIMD vector
  (v128.store offset=144 ($70))
  (v128.store offset=160 ($52))
  (v128.store offset=176 ($51))
  (v128.store offset=96  ($50))
  (v128.store offset=112 ($49))

  (i32.store16 ...                ;; per-lane scalar work:
    (i32.mul                       ;; load u16, mul, store u16
      (i32.load16_u ...)
      (i32.load16_u ...)))

  (local.set $49 (v128.load offset=112 ...))  ;; reload vectors
  (local.set $50 (v128.load offset=96  ...))

  (br_if $label3                  ;; iterate 16 times
    (i32.ne (local.tee $10 (i32.add (local.get $10) (i32.const 1)))
            (i32.const 16))))
```

Per pixel-group: ~64 scalar `i32.mul`s + ~12 v128 spill round-trips. Where 4
`i16x8.mul` instructions should be.

`i16x8.mul` count in apply_exact body: **0**. (There are 30 globally in the
binary, all inside `stbi_load`'s auto-vectorized loops.)

## Bisecting through the abstraction stack

Theory was "vectorclass's emulated Vec16us multiply scalarizes." Test:

```cpp
// Direct wasm intrinsic
extern "C" v128_t test1(v128_t a, v128_t b) { return wasm_i16x8_mul(a, b); }
// → 1 i16x8.mul ✓

// simde 128-bit
extern "C" __m128i test2(__m128i a, __m128i b) { return _mm_mullo_epi16(a, b); }
// → 1 i16x8.mul ✓ (after upgrading simde 0.7.3→0.8.2)

// vectorclass 128-bit
extern "C" Vec8us test3(Vec8us a, Vec8us b) { return a * b; }
// → 1 i16x8.mul ✓

// vectorclass 256-bit (emulated)
extern "C" Vec16us test4(Vec16us a, Vec16us b) { return a * b; }
// → 0 i16x8.mul, scalar i32.mul loop ✗  ← scalarized

// simde 256-bit, NO vectorclass
extern "C" __m256i test5(__m256i a, __m256i b) { return _mm256_mullo_epi16(a, b); }
// → 0 i16x8.mul, scalar i32.mul loop ✗  ← scalarized HERE TOO
```

So vectorclass isn't the culprit. **simde's 256-bit fan-out on wasm** is.

## Reading simde's source

`FDS/simde/x86/avx2.h:3550`, `simde_mm256_mullo_epi16`:

```c
simde_mm256_mullo_epi16 (simde__m256i a, simde__m256i b) {
  #if defined(SIMDE_X86_AVX2_NATIVE)
    return _mm256_mullo_epi16(a, b);
  #else
    simde__m256i_private a_ = ..., b_ = ..., r_;

    SIMDE_VECTORIZE
    for (size_t i = 0 ; i < 16 ; i++) {
      r_.i16[i] = a_.i16[i] * b_.i16[i];
    }

    return simde__m256i_from_private(r_);
  #endif
}
```

Just a scalar loop. clang's auto-vectorizer warns `loop not vectorized` and
emits the i32.mul loop we saw.

Compare `simde_mm256_add_epi16` in the same file — it has the right pattern:

```c
#if SIMDE_NATURAL_INT_VECTOR_SIZE_LE(128)
  r_.m128i[0] = simde_mm_add_epi16(a_.m128i[0], b_.m128i[0]);
  r_.m128i[1] = simde_mm_add_epi16(a_.m128i[1], b_.m128i[1]);
#elif ...
```

`SIMDE_NATURAL_INT_VECTOR_SIZE_LE(128)` is true on wasm (no native 256-bit
SIMD), so the 128-bit fan-out fires. Both 128-bit `simde_mm_add_epi16` calls
hit a wasm clause that maps to `wasm_i16x8_add`. Result: 2 SIMD adds. Works.

The `mullo_epi16` function is just **missing this clause**. Same simde
version, same file, sibling intrinsics — somebody never got around to it.

## The fix

Add the same clause to `simde_mm256_mullo_epi16`:

```c
#if SIMDE_NATURAL_INT_VECTOR_SIZE_LE(128)
  r_.m128i[0] = simde_mm_mullo_epi16(a_.m128i[0], b_.m128i[0]);
  r_.m128i[1] = simde_mm_mullo_epi16(a_.m128i[1], b_.m128i[1]);
#else
  SIMDE_VECTORIZE
  for ...
#endif
```

Plus the same for `simde_mm256_srli_epi16` (also missing). For srli, can't
use the cross-platform fan-out because `simde_mm_srli_epi16` resolves to a
macro on arm64 NEON that requires a compile-time-constant `imm8`, and the
inline boundary at simde_mm256_srli_epi16 doesn't propagate the constant
through. Solution: gate on `SIMDE_WASM_SIMD128_NATIVE` and call
`wasm_u16x8_shr` directly via `m128i_private[i].wasm_v128`.

## Result

Native unchanged. Wasm:

| | wasm before | wasm after | speedup |
|---|---|---|---|
| synthetic-quad | 4.597 ms | 1.011 ms | 4.55× |
| city@t=1961 | 28.118 ms | 16.175 ms | 1.74× |
| greets@t=600 | 9.275 ms | 3.519 ms | 2.64× |

Wasm matches native within 1% on city + greets. On synthetic-quad wasm is
**12% faster than native**, presumably because the i16x8 wasm SIMD path is
slightly tighter than simde's mapping to NEON via 256→128 fan-out.

apply_exact body: 3603 → 3177 lines. `i16x8.mul`: 0 → 8. Spill smoking-gun
(`i32.store16`/`i32.load16_u`): 4/8 → 0/0.

## Audit

Same script that pulled `_mm256_mullo_epi16` from simde — find function
bodies in avx2.h with `SIMDE_VECTORIZE` but no `SIMDE_NATURAL_INT_VECTOR_SIZE_LE`
or `SIMDE_WASM_SIMD128_NATIVE` clause. 65 of 158 simde_mm256_* functions
match.

Of those 65, only the 8/16-bit-lane ones actually scalarize on wasm:
clang auto-vectorizes the 32/64-bit-lane scalar loops fine. Confirmed
broken by minimal-repro warnings:

```
_mm256_mullo_epi16   ← patched (rasterizer)
_mm256_srli_epi16    ← patched (rasterizer)
_mm256_avg_epu8      ← patched (preemptive)
_mm256_avg_epu16     ← patched (preemptive)
_mm256_mulhi_epi16   ← patched (preemptive)
_mm256_mulhi_epu16   ← patched (preemptive)
_mm256_mulhrs_epi16  ← patched (preemptive)
_mm256_sign_epi8     ← patched (preemptive)
_mm256_sign_epi16    ← patched (preemptive)
```

## Things that didn't work / weren't the cause

- **Vectorclass upgrade** (2.01.03 → 2.02.03). Vec16us multiply is byte-identical
  across versions. Vectorclass treats wasm as "SSE2 without AVX2" via the
  `e`-suffixed emulated variants; never added wasm clauses upstream.
- **Dropping vectorclass entirely**. ~1500-2000 lines of rewriting; doesn't
  fix the proximate cause (simde's 256-bit gap is hit either way unless we
  also stop using `_mm256_*` and manually manage two 128-bit halves
  everywhere).
- **simde master upgrade**. 315 commits ahead of 0.8.2 but `_mm256_mullo_epi16`
  / `_mm256_srli_epi16` still scalar-loop-only. Same patches still needed.
- **Algorithmic colorize rewrite** (the originally-planned `ColorizeBlend`
  template-dispatched struct). Would have worked but at higher cost than
  fixing simde once.

## simde upstream status

Their CONTRIBUTING.md: "we cannot accept contributions written by LLMs."
Hard policy. Direct PR is off the table.

Path: file an issue with reproduction (bench data, disasm, observation
about the missing clause vs `_mm256_add_epi16`). Maintainer can write the
patch from the report. Open issues #86, #776, #968 indicate the project
*wants* this work; they just can't accept LLM-authored patches.

## Toolbox

| tool | use |
|---|---|
| `make bench-scenes-{native,wasm}` | run the variant matrix |
| `wasm-dis` (binaryen) | disassemble .wasm to .wat |
| `emcc -O3 -msimd128 ... -s STANDALONE_WASM` | compile minimal repros |
| `clang -Wpass-failed=transform-warning` | implicit; surfaces "loop not vectorized" |
| `find body of fn$X` via grep | locate template instantiations in disasm |
| op-skip `BENCH_VARIANT` | attribute time to specific inner-loop ops |

## Loose ends

- Audit didn't run on `simde/x86/avx.h`, `sse4.1.h`, `ssse3.h`, etc. — only
  avx2.h. Other 256-bit ops likely have similar gaps.
- Texture gather is still ~2× slower on wasm than native (+0.4-1.3ms tax),
  but that's intrinsic — wasm has no native gather instruction. Could
  hand-write a sequential-scalar gather that's better than simde's emulation,
  but the absolute cost is small.
- Bench has noise ±50ms or so on city; results stable enough for our
  conclusions but not for sub-1% claims.
- `_mm256_blend_epi16` and a few others were in the audit list but
  auto-vectorize fine — sometimes the SIMDE_VECTORIZE pragma is enough.
  Pattern is "varies by lane width."
