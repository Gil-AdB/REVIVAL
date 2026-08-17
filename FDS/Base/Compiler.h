#pragma once
// Compiler-portability macros for the GCC/clang extensions this tree uses.
//
// On clang and GCC (macOS arm64, MinGW-w64) every macro below expands to
// EXACTLY the token sequence the call sites used before this header existed,
// so the preprocessed output — and therefore the generated code — is
// unchanged. Only MSVC's `cl.exe` gets a different expansion.
//
// This matters more than the usual "portability macro" boilerplate, because
// several of these attributes are CODEGEN-LOAD-BEARING in this engine, not
// decoration: the noinline markers exist because inlining those bodies
// measurably moved frame times and, through -ffp-contract=fast, moved the
// scene pins. Dropping them on any compiler is a behaviour change, so the
// MSVC arm maps them to the closest real equivalent rather than to nothing.
//
// MSVC status: UNTESTED. No MSVC compiler was available when this was
// written. The mappings are from Microsoft's documentation.

// ---------------------------------------------------------------------------
// Function attributes
// ---------------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(__clang__)
	#define FDS_NOINLINE       __declspec(noinline)
	#define FDS_ALWAYS_INLINE  __forceinline
	// MSVC has no printf-format checking attribute. SAL's _Printf_format_
	// string_ works only on the parameter, not as a function attribute, so
	// the check is simply absent there.
	#define FDS_PRINTF_FMT(fmtIdx, argIdx)
#else
	#define FDS_NOINLINE       __attribute__((noinline))
	#define FDS_ALWAYS_INLINE  __attribute__((always_inline))
	#define FDS_PRINTF_FMT(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#endif

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------
#if defined(_MSC_VER) && !defined(__clang__)
	#include <intrin.h>
	#include <cmath>
	// __popcnt is an SSE4.2/POPCNT instruction intrinsic. The x86-64 build
	// already requires AVX2 (see the top-level CMakeLists), which implies
	// POPCNT, so this is always available on a target this engine supports.
	#define FDS_POPCOUNT(x)  ((int)__popcnt((unsigned int)(x)))
	// std::fmaf is the exact IEEE fused multiply-add, same contract as
	// __builtin_fmaf. It is NOT interchangeable with `a*b+c`: the call sites
	// sit inside FP_CONTRACT_OFF scopes precisely because they need the
	// single-rounding result, and the scene pins are keyed to it.
	#define FDS_FMAF(a, b, c) std::fmaf((a), (b), (c))
#else
	#define FDS_POPCOUNT(x)  __builtin_popcount(x)
	#define FDS_FMAF(a, b, c) __builtin_fmaf((a), (b), (c))
#endif
