#!/usr/bin/env python3
"""The per-pixel ladder's sinks were per-stage, so a late cut let the compiler
dead-strip every earlier stage's output (stages 9/10 measured LESS than 8).
Replace them with a RUNNING accumulator: every boundary adds its own outputs to
`ablKeep`, and the cut sinks the whole running value, so stage k retains
everything above k. Bias: one fadd per boundary crossed (~0.0006 Gi/stage)."""
K = '/Users/gil-ad/work/rev-deflight/FDS/RENDER/DeferredSurfaceKernel.cpp'
s = open(K).read()

s = s.replace('''#if FDS_PIX_ABLATE
#define PIX_ABL_CUT(stage, expr) \\
    if constexpr ((FDS_PIX_ABLATE) == (stage)) { ablSink += (expr); continue; }
#else
#define PIX_ABL_CUT(stage, expr)  ((void)0)
#endif''','''#if FDS_PIX_ABLATE
// The sink is CUMULATIVE: each boundary adds its own outputs to a running
// per-pixel accumulator and the cut sinks that, so a late cut cannot let the
// compiler dead-strip an early stage's work. Costs one fadd per boundary
// crossed, which is why every stage is quoted against the same ladder.
#define PIX_ABL_CUT(stage, expr) \\
    ablKeep += (expr); \\
    if constexpr ((FDS_PIX_ABLATE) == (stage)) { ablSink += ablKeep; continue; }
#else
#define PIX_ABL_CUT(stage, expr)  ((void)0)
#endif''', 1)

# declare the running accumulator at the top of each pixel iteration
old = '''			const size_t i = size_t(py) * XRes + px;
			const word zEnc = ZPage16[i];
			if (zEnc == 0) continue;  // pixel not touched by Mekalele
			PIX_ABL_CUT(1, float(zEnc));'''
new = '''			const size_t i = size_t(py) * XRes + px;
			const word zEnc = ZPage16[i];
			if (zEnc == 0) continue;  // pixel not touched by Mekalele
#if FDS_PIX_ABLATE
			float ablKeep = 0.0f;
#endif
			PIX_ABL_CUT(1, float(zEnc));'''
assert s.count(old) == 1
s = s.replace(old, new, 1)
open(K, 'w').write(s)
print("sinks fixed")
