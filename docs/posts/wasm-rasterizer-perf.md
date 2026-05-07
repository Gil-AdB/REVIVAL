# Why was our wasm rasterizer 4× slower than native? Two missing lines in simde.

Drafted on the master branch of [REVIVAL/FLOOD](https://github.com/Gil-AdB/REVIVAL),
a 1998 demoscene production being revived on modern platforms. Software
rasterizer in C++17, native arm64 macOS via simde→NEON, wasm via
emscripten + simde→wasm SIMD128. The story is a decent argument for keeping
a working bench harness around, and a less-decent argument for trusting
your library dependencies to vectorize for you.

## The puzzle

The City scene at 1920×1080 ran at ~16 ms/frame on native and ~28 ms/frame
on wasm. A consistent **4-5× slowdown** showed up across our three
benchmark workloads:

| workload          | native | wasm  | ratio |
|-------------------|--------|-------|-------|
| synthetic-quad    | 1.15   | 4.60  | 4.0×  |
| city@t=1961       | 16.29  | 28.12 | 1.7×  |
| greets@t=600      | 3.48   | 9.27  | 2.7×  |

Wasm SIMD128 isn't supposed to be that much slower than native NEON. They
have similar instruction counts for the operations we use, similar
cycle costs on modern engines (V8, JSC). A 2× wasm tax was reasonable to
expect. 4× was not.

## First instinct: blame the gather

The rasterizer is fundamentally a tile-based texture-mapped triangle
filler. It does perspective-correct UV interpolation, gouraud color
modulation, Z-test, and writes pixels with a masked store. The
suspect-of-record for "wasm slower than native" in this kind of code
is usually **the texture gather** — wasm has no `_mm_i32gather_*`
equivalent, simde emulates by per-lane scalar loads.

So I started writing a `ColorizeBlend` template-dispatched struct that
would replace the per-pixel gouraud blend with a hand-rolled wasm path,
hoping to shave a chunk off the colorize step. About 30 minutes in,
my collaborator pushed back: *don't refactor an algorithm before you
know what's actually slow*. Build a bench harness first.

## The bench harness — op-skip variants

The trick is *attribution*. Total frame time is a sum of many costs:
edge tests, perspective division, texture gather, color blend, Z-test,
maskstore, etc. If you only have one number, you can't tell which one
is the bottleneck. So we built a harness where each rasterizer
operation could be **stubbed out at compile time** via `BENCH_VARIANT`
macros:

- `no-texture` — replaces the gather with a constant white. Isolates gather cost.
- `no-perspective` — drops the `* p_z` perspective divide from UV mul.
- `no-z` — skips Z-test/blend/store. Pixels still get gathered + written.
- `no-color` — skips the gouraud blend. Texture sample feeds the maskstore directly.
- `no-maskstore` — replaces masked store with unmasked. Writes outside the triangle, but isolates the store cost.

Each variant is its own build dir, its own binary. Run all of them
across our three real workloads (a synthetic full-screen quad, a city
scene at a fixed timestamp, a greets scene at a fixed timestamp), and
the **delta from `full` to `no-X`** attributes time to operation X.

The matrix:

```
NATIVE
                 full     no-tex   no-persp no-mask  no-z     no-color
synthetic-quad   1.154    0.741    1.084    1.061    0.976    0.940
city@t=1961     16.286   14.852   15.763   16.233   16.342   15.369
greets@t=600     3.480    3.128    3.369    3.507    3.429    3.243

WASM
                 full     no-tex   no-persp no-mask  no-z     no-color
synthetic-quad   4.522    3.700    4.504    4.420    4.678    0.729  ← !
city@t=1961     28.672   25.675   28.347   28.227   34.913   16.187  ← !
greets@t=600     9.414    8.391    9.032    9.187   10.698    5.330  ← !
```

The last column on wasm is the smoking gun. **`no-color` is faster
than every other variant on every workload, by *huge* margins.**
City: 28.7 → 16.2 ms (12.5 ms saved by skipping colorize). Synthetic:
4.5 → 0.7 ms (84% of total wasm time was in colorize). Native shows
nothing comparable — `no-color` saves 0.21 ms on synthetic, 0.92 ms
on city.

So the wasm tax is concentrated in the **color blend**, not the
texture gather. Time to figure out why colorize is hot on wasm but
not native.

## What is colorize?

It's one line in `SimdHelpers.h`:

```cpp
template <uint8_t Shift = 0>
inline Vec32uc colorize(Vec32uc color1, Vec32us color2) {
    return compress((extend(color1) * (color2 >> Shift)) >> 8);
}
```

8 RGBA pixels per call. Take the 32-byte texture sample (`Vec32uc`),
zero-extend each byte to a 16-bit value (`extend`), multiply elementwise
by the gouraud-interpolated 16-bit blend (`color2`), shift right by 8,
then saturating-narrow back to 32 bytes (`compress`). On x86 AVX2 it's
maybe 5 SIMD ops. On native arm64 NEON via simde, similar. On wasm —
?

## wasm-dis is your friend

The bench tooling lives at `make bench-scenes-wasm`. Each variant
produces a `DEMO_snapshot.wasm`. Disassemble with binaryen's `wasm-dis`:

```sh
wasm-dis build-wasm-bench/full/DEMO/DEMO_snapshot.wasm > out.wat
```

Symbols are stripped by default. Rebuild with `-g2` to keep the wasm
`name` section, then find your function:

```sh
grep -nE "^ \(func \\\$.*TheOtherBarry<.*TBlendMode.291" out.wat
```

(That's the most-used template instantiation — `TBlendMode = OVERWRITE,
TTextureMode = NORMAL`.) The rasterizer's hot inner-loop body is in
this function, ~3500 lines of `.wat`.

I expected to find a chain like:
```
i8x16.shuffle    ;; extend low half
i8x16.shuffle    ;; extend high half
i16x8.mul        ;; the multiply
i16x8.mul
i16x8.shr_u 8
i16x8.shr_u 8
i8x16.narrow_i16x8_u  ;; compress
```

Maybe ~14 wasm SIMD ops, given the 256→128 fan-out. Instead I found
this:

```wat
(loop $label3
  ;; spill 6 SIMD vectors to stack
  (v128.store offset=128  (local.get $5) (local.get $71))
  (v128.store offset=144  (local.get $5) (local.get $70))
  (v128.store offset=160  (local.get $5) (local.get $52))
  (v128.store offset=176  (local.get $5) (local.get $51))
  (v128.store offset=96   (local.get $5) (local.get $50))
  (v128.store offset=112  (local.get $5) (local.get $49))

  ;; per-lane scalar 16-bit multiply, address keyed off iter counter $10
  (i32.store16 (compute address)
    (i32.mul
      (i32.load16_u (load address 1))
      (i32.load16_u (load address 2))))

  (local.set $49 (v128.load offset=112 (local.get $5)))
  (local.set $50 (v128.load offset=96  (local.get $5)))

  (br_if $label3
    (i32.ne (local.tee $10 (i32.add (local.get $10) (i32.const 1)))
            (i32.const 16)))
)
```

**That's a scalar loop**. 16 iterations of `i32.mul` on lane-by-lane
loads from spilled SIMD vectors. Four such loops in the function, one
per Vec32us 128-bit chunk. Roughly **64 scalar `i32.mul`s plus 12
v128 spill round-trips per 8-pixel colorize call**, where 4 `i16x8.mul`
instructions should suffice.

The histogram of ops in the body confirmed it:

| op                    | count |
|-----------------------|-------|
| `i16x8.mul`           | 0     |
| `i32.mul` (in body)   | 8 (looped 16×) |
| `i32.store16`         | 4     |
| `i32.load16_u`        | 8     |
| `v128.store/load`     | many  |

Zero SIMD multiplies in the rasterizer's inner loop. The compiler
chose to scalarize.

## Bisecting the abstraction stack

The rasterizer goes through three layers: vectorclass (Agner Fog's VCL,
provides `Vec*` types and operator overloads), simde (provides `_mm_*`
and `_mm256_*` intrinsics on non-x86 targets), and the native target
SIMD (wasm SIMD128 via clang). Where does the scalarization happen?

Minimal isolation tests for each layer:

```cpp
// 1. Direct wasm intrinsic
extern "C" v128_t test1(v128_t a, v128_t b) { return wasm_i16x8_mul(a, b); }
// → 1 i16x8.mul ✓

// 2. simde 128-bit
extern "C" __m128i test2(__m128i a, __m128i b) { return _mm_mullo_epi16(a, b); }
// → 1 i16x8.mul ✓

// 3. vectorclass 128-bit
extern "C" Vec8us test3(Vec8us a, Vec8us b) { return a * b; }
// → 1 i16x8.mul ✓

// 4. vectorclass 256-bit (emulated as two 128-bit halves)
extern "C" Vec16us test4(Vec16us a, Vec16us b) { return a * b; }
// → scalar i32.mul loop ✗

// 5. simde 256-bit, no vectorclass
extern "C" __m256i test5(__m256i a, __m256i b) { return _mm256_mullo_epi16(a, b); }
// → scalar i32.mul loop ✗
```

So vectorclass is fine. The 128-bit simde path is fine. The **256-bit
simde path scalarizes on wasm** even though wasm SIMD is 128-bit and
the natural lowering would be two 128-bit operations.

## Reading simde's source

`FDS/simde/x86/avx2.h:3550`, `simde_mm256_mullo_epi16`:

```c
simde_mm256_mullo_epi16 (simde__m256i a, simde__m256i b) {
  #if defined(SIMDE_X86_AVX2_NATIVE)
    return _mm256_mullo_epi16(a, b);
  #else
    simde__m256i_private a_ = simde__m256i_to_private(a),
                          b_ = simde__m256i_to_private(b),
                          r_;

    SIMDE_VECTORIZE
    for (size_t i = 0; i < 16; i++) {
      r_.i16[i] = a_.i16[i] * b_.i16[i];
    }

    return simde__m256i_from_private(r_);
  #endif
}
```

The non-AVX2 fallback is *only* a `SIMDE_VECTORIZE` scalar loop. clang
on wasm fails to auto-vectorize that loop and emits the scalar
`i32.mul` chain we saw. There is no native wasm path.

Look at a sibling intrinsic, `simde_mm256_add_epi16`, in the same file:

```c
simde_mm256_add_epi16 (simde__m256i a, simde__m256i b) {
  ...
    #if SIMDE_NATURAL_INT_VECTOR_SIZE_LE(128)
      r_.m128i[0] = simde_mm_add_epi16(a_.m128i[0], b_.m128i[0]);
      r_.m128i[1] = simde_mm_add_epi16(a_.m128i[1], b_.m128i[1]);
    #elif defined(SIMDE_VECTOR_SUBSCRIPT_OPS)
      r_.i16 = a_.i16 + b_.i16;
    #else
      SIMDE_VECTORIZE
      for (size_t i = 0 ; ...) ...
    #endif
  ...
}
```

`SIMDE_NATURAL_INT_VECTOR_SIZE_LE(128)` is true on wasm, so the
fan-out path fires, and each 128-bit `simde_mm_add_epi16` *does*
have a wasm clause that emits `wasm_i16x8_add`. Result: `_mm256_add_epi16`
produces 2× SIMD `i16x8.add` on wasm. Works as expected.

`_mm256_mullo_epi16` is just **missing this clause**. Same simde
version, same file, sibling intrinsics, somebody never got around to it.

## The fix

Five lines:

```c
+    #if SIMDE_NATURAL_INT_VECTOR_SIZE_LE(128)
+      r_.m128i[0] = simde_mm_mullo_epi16(a_.m128i[0], b_.m128i[0]);
+      r_.m128i[1] = simde_mm_mullo_epi16(a_.m128i[1], b_.m128i[1]);
+    #else
       SIMDE_VECTORIZE
       for (size_t i = 0; i < 16; i++) {
         r_.i16[i] = a_.i16[i] * b_.i16[i];
       }
+    #endif
```

A second similar patch on `_mm256_srli_epi16` (which had the same gap).
Together: 23 lines added across two functions in `FDS/simde/x86/avx2.h`.

## The result

| workload         | native | wasm before | wasm after | wasm vs native |
|------------------|--------|-------------|------------|----------------|
| synthetic-quad   | 1.154  | 4.597       | **1.011**  | wasm 12% faster |
| city@t=1961      | 16.286 | 28.118      | **16.175** | within 0.7%    |
| greets@t=600     | 3.480  | 9.275       | **3.519**  | within 1.1%    |

Wasm at native parity across all three workloads. City alone saved
12.5 ms/frame. apply_exact's `i16x8.mul` count went 0 → 8. The four
scalar mul loops are gone.

## Methodology

The thing that worked, in order:

1. **Stop refactoring on hunches.** The first impulse was
   "rewrite colorize." It would have shipped, would have gotten ~30%
   on wasm, and would have left the underlying issue in place for the
   next 16-bit-lane operation we ever wrote.

2. **Op-skip variants are absurdly powerful.** Compile a variant with
   each rasterizer op stubbed, run the bench, look at the deltas.
   The matrix attributes time to ops with no profiler, no flame chart,
   no per-call instrumentation. ~150 LOC of bench harness, reusable
   for every future perf question on this codebase.

3. **wasm-dis is the floor.** When the variant data points at a
   specific op, look at the actual emitted wasm. The pattern of
   "v128.store; scalar i32 work; v128.load" is a fingerprint for
   *something didn't vectorize*. Once you know that fingerprint, you
   recognize it instantly across codebases.

4. **Trust the bench, distrust your hypothesis.** The first three
   theories were wrong: gather, then vectorclass's emulated multi-level
   classes, then "we need to bypass vectorclass." The actual answer
   was inside simde, at the leaf where I'd assumed the library was
   competent.

## Things I didn't do

A few cul-de-sacs worth flagging because someone in the same situation
might be tempted by them:

- **Upgrade simde to master**. simde master is 315 commits past v0.8.2
  but `_mm256_mullo_epi16` *still* has only the scalar fallback there.
  The gap is upstream and not yet fixed.

- **Upgrade vectorclass**. Vec16us::operator* is byte-identical between
  v2.01.03 and v2.02.03. Vectorclass treats wasm as "SSE2 without AVX2"
  and never added wasm clauses. Not the lever.

- **Drop vectorclass entirely**. ~1500-2000 LOC rewrite to call simde
  intrinsics directly. Doesn't fix the issue (the simde-256 fan-out
  is still broken; you'd hit it via direct `_mm256_*` calls). Save it
  for a different decision.

- **Hand-roll a `colorize_wasm`** with `wasm_i16x8_extmul_*_u8x16`. Would
  have worked and produced a clean win, at the cost of a custom path
  to maintain forever. The simde patch fixes the same problem for every
  consumer of every 16-bit-lane 256-bit AVX2 intrinsic at the same time.

## The PR situation

simde's CONTRIBUTING.md states: *"we cannot accept contributions written
by LLMs."* It's a written policy, plain text, paragraph two. The patch
itself was discussed with an LLM-pair-programming partner, so we won't
PR it directly. Path forward: file an issue with the bench data + the
wasm-dis output + the observation that `_mm256_add_epi16` already has
the right clause and `_mm256_mullo_epi16` doesn't. Maintainer can write
the five-line patch as their own work in fifteen minutes. Open issues
[#86](https://github.com/simd-everywhere/simde/issues/86) and
[#776](https://github.com/simd-everywhere/simde/issues/776) are exactly
this category of work and indicate the project wants the report.

## Audit follow-up

The same audit script over `simde/x86/avx2.h` flagged 65 of 158
intrinsics with only a `SIMDE_VECTORIZE` scalar fallback. Spot-checking
each one for actual scalarization showed it's specifically the **8-bit
and 16-bit lane operations** that fail clang's auto-vectorizer on
wasm — 32-bit and 64-bit lane ops auto-vectorize fine. We patched
seven more preemptively (`avg_epu8`, `avg_epu16`, `mulhi_epi16`,
`mulhi_epu16`, `mulhrs_epi16`, `sign_epi8`, `sign_epi16`); none are
in our current rasterizer hot path, but they'd bite the next time
someone writes per-pixel fixed-point lighting. `simde/x86/avx.h`,
`ssse3.h`, `sse4.1.h`, and `sse2.h` are clean.

## Tools

- **`make bench-scenes-{native,wasm}`** — variant matrix runner.
  ~5 min native, ~30 min wasm.
- **`wasm-dis`** (binaryen, ships with emscripten) — disassembles
  `.wasm` to `.wat`.
- **`emcc -O3 -msimd128 -DSIMDE_ENABLE_NATIVE_ALIASES -s STANDALONE_WASM`**
  for minimal-repro tests of single intrinsics.
- **`-Wpass-failed=transform-warning`** is implicit; clang prints
  *"loop not vectorized"* for the scalarization fingerprint.

## Why bother writing this up

The methodology is the value. Variant-bench + disasm-grep + minimal-repro
isolation got us from "wasm is mysteriously 4× slower" to a five-line
patch in a few hours of focused work, while taking three wrong turns
that the data immediately corrected. None of the steps required deep
SIMD expertise — they required *patient attribution*. Anyone with a
build system and a profiler-substitute (variants are the substitute)
can do this.

The library-dependency lesson is the second prize. simde and vectorclass
are excellent libraries, but neither was written *for* wasm, and the
edges show up at exactly the points where you've stopped paying
attention. Look at what your tools actually produce. Treat
`SIMDE_VECTORIZE` as "I'll let clang figure it out," and keep checking.
