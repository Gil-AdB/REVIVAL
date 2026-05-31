#pragma once

// Block-scoped FMA-contraction control macros. The build defaults to
// -ffp-contract=fast (cross-statement FMA fusion) for the perf win in
// vector hot loops; some specific code blocks need to opt OUT because
// the 1-ULP shifts produced by FMA fusion cause visible artifacts
// (e.g. per-pixel attribute interpolation inside apply_exact —
// neighboring texels sampled at high-contrast triangle edges produce
// horizon shimmer; bisected to 7e4c2ac, diagnosed 2026-05-31).
//
// Compiler support matrix:
//   clang:   full block-level support via `#pragma clang fp contract`.
//   gcc:     ISO `#pragma STDC FP_CONTRACT` supports OFF/ON at block
//            scope; FAST has no block form (would need an attribute
//            on the enclosing function). FP_CONTRACT_FAST is a no-op
//            on GCC — any hot path using it MUST also be inside a
//            function decorated with
//            __attribute__((optimize("-ffp-contract=fast"))) to
//            actually get FAST under GCC.
//   msvc:    `#pragma fp_contract(on)` is closest to FAST but lacks
//            cross-statement semantics; we map FAST to "on" there
//            and accept the lesser perf.
//   other:   no-ops; rely on global -ffp-contract from the build.
//
// Current build target (mac + Emscripten) is clang-only, so the clang
// path covers every shipping configuration. The GCC/MSVC fallbacks
// exist so the codebase doesn't break under those compilers if anyone
// ports it — they just lose some of the perf scoping.
//
// Usage:
//   void f() {
//     FP_CONTRACT_OFF  // MUST be at the start of a compound statement
//     // FP math that must NOT auto-fuse to FMA (e.g. per-pixel UV interp
//     // that samples textures — 1-ULP UV shifts can land on neighboring
//     // texels and produce visible edge color flicker)
//   } // OFF auto-scopes to the enclosing block; no explicit close needed
//
// IMPORTANT: clang's `#pragma clang fp contract` can only appear at file
// scope or at the START of a compound statement (block). Putting one
// mid-block raises -Wpragma-system-header / errors out. To opt out of
// part of a function, factor that part into its own `{ ... }` block:
//
//   void f() {
//     /* code under build-default contraction */
//     {
//       FP_CONTRACT_OFF
//       /* this sub-block opts out */
//     }
//     /* back to default */
//   }
//
// To selectively re-enable FAST inside an OFF-scoped block, nest:
//   FP_CONTRACT_OFF
//   { FP_CONTRACT_FAST  /* hot math safe to fuse */ }
//   /* outer block still OFF here */
//
// FP_CONTRACT_ON resets to the C++ default contraction (single-expression),
// NOT to the build's -ffp-contract setting. Most callers won't need it
// because scope-end already restores enclosing context.

#if defined(__clang__)
  #define FP_CONTRACT_OFF  _Pragma("clang fp contract(off)")
  #define FP_CONTRACT_ON   _Pragma("clang fp contract(on)")
  #define FP_CONTRACT_FAST _Pragma("clang fp contract(fast)")
#elif defined(__GNUC__)
  #define FP_CONTRACT_OFF  _Pragma("STDC FP_CONTRACT OFF")
  #define FP_CONTRACT_ON   _Pragma("STDC FP_CONTRACT ON")
  // No block-level FAST on GCC; rely on function-level optimize attribute.
  #define FP_CONTRACT_FAST /* no-op on GCC */
#elif defined(_MSC_VER)
  #define FP_CONTRACT_OFF  __pragma(fp_contract(off))
  #define FP_CONTRACT_ON   __pragma(fp_contract(on))
  #define FP_CONTRACT_FAST __pragma(fp_contract(on))
#else
  #define FP_CONTRACT_OFF
  #define FP_CONTRACT_ON
  #define FP_CONTRACT_FAST
#endif
