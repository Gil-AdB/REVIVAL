#!/usr/bin/env python3
"""Round-1 candidate patches for DeferredLighting-call. Each is byte-null or
bit-exact by construction; each gets its own FeatureFlags hatch where the
restructure is non-trivial."""
import sys, re

K = '/Users/gil-ad/work/rev-deflight/FDS/RENDER/DeferredSurfaceKernel.cpp'
F = '/Users/gil-ad/work/rev-deflight/FDS/Base/FeatureFlags.def'

def sub(path, old, new, n=1):
    s = open(path).read()
    assert s.count(old) == n, (path, s.count(old), old[:80])
    open(path, 'w').write(s.replace(old, new, n))

# ── 1. FeatureFlags hatches ─────────────────────────────────────────────────
sub(F,
'''FDS_FLAG_BOOL(deferred_checkerboard,''',
'''FDS_FLAG_BOOL(deferred_lm_addr_skip,    "FDS_DEFERRED_LM_ADDR_SKIP",    1,                              "deferred", "Resolve the per-pixel static-shadow LIGHTMAP ADDRESS only when the lightmap kernel is actually live. resolveCubeAtten reads the PixelLightmap exclusively behind `useLightmap && pl.lm`, and useLightmap IS lmKernelEnabled -- so whenever the lightmap kernel is off (greets ships --shadow_dynamic on with --shadow_lm_dynamic off, which is exactly that case) the two G-buffer plane loads, the scene-table index and the two pointer chases run per pixel for a value nothing reads, and their four results stay live across the whole pixel body. OFF restores the unconditional resolve. Byte-null by construction: the value is consumed only under the flag that gates it.")
FDS_FLAG_BOOL(deferred_cube_direct,     "FDS_DEFERRED_CUBE_DIRECT",     1,                              "deferred", "Call CubeShadow_Sample directly from the omni loop when resolveCubeAtten can only forward to it. The wrapper takes 20 arguments and carries the lightmap arm, the two lightmap debug-recompute arms and the Depth-mode slope-bias arm; with the lightmap kernel off and ShadowMode::PolyId (greets' shipping configuration) every one of those is unreachable and the wrapper is pure frame + argument setup around one call. The direct form passes 8 arguments instead of 20 and drops wx/wy/wz/lenInv/nGeo/the PixelLightmap/the CubeAttenFlags reference from the innermost body's live set. Predicate is hoisted per tile (flags) x per pixel (the receiver's shadow id); anything it does not cover still goes through resolveCubeAtten. BIT-EXACT by construction -- same callee, same arguments, in the arm it replaces.")
FDS_FLAG_BOOL(deferred_fill_hdr_skip,   "FDS_DEFERRED_FILL_HDR_SKIP",   1,                              "deferred", "In the checkerboard/quarter FILL (wave 2), skip the LDR sharp-reconstruction accumulators when the frame is HDR. slB/slG/slR are read only by the `haveOwn && nsharp > 0 && !hdrWrite` arm, so under --hdr they are three DIVISIONS per compatible neighbour computed for a value nothing reads (two neighbours per pixel in checkerboard). Also hoists --quarter_tex_sharp out of the per-pixel body. Byte-null by construction.")
FDS_FLAG_BOOL(deferred_checkerboard,''')

# ── 2. the lightmap-address skip ────────────────────────────────────────────
sub(K,
'''			const PixelLightmap pixelLM = resolvePixelLightmap(gb, i, ctx.Sc);''',
'''			// --deferred_lm_addr_skip: the address is read ONLY behind
			// resolveCubeAtten's `useLightmap && pl.lm` guard and useLightmap is
			// lmKernelEnabled, so with the lightmap kernel off this whole resolve
			// (two G-buffer planes, a scene-table index, two pointer chases) is
			// dead — and its four results otherwise stay live across the entire
			// pixel body. Byte-null by construction.
			const PixelLightmap pixelLM = (lmKernelEnabled || !lmAddrSkip)
			    ? resolvePixelLightmap(gb, i, ctx.Sc)
			    : PixelLightmap{};''')

# ── 3. the direct cube tap ──────────────────────────────────────────────────
sub(K,
'''	const bool lmKernelEnabled  = !fds::FeatureFlags::shadow_dynamic()
	                            || fds::FeatureFlags::shadow_lm_dynamic();''',
'''	const bool lmKernelEnabled  = !fds::FeatureFlags::shadow_dynamic()
	                            || fds::FeatureFlags::shadow_lm_dynamic();
	// --deferred_lm_addr_skip / --deferred_cube_direct:
	// hoisted here so the per-pixel body reads a register, not the flag array.
	const bool lmAddrSkip       = fds::FeatureFlags::deferred_lm_addr_skip();''')

sub(K,
'''	    /*shadowMode           */ g_shadowMode.load(std::memory_order_relaxed),
	};''',
'''	    /*shadowMode           */ g_shadowMode.load(std::memory_order_relaxed),
	};
	// --deferred_cube_direct: the tile half of the predicate. resolveCubeAtten
	// reduces to one CubeShadow_Sample call — same arguments, no bias math —
	// exactly when the lightmap arm is unreachable (lmKernelEnabled off), the
	// tap is not short-circuited, and the mode is PolyId. The per-pixel half is
	// `surfaceShadowId >= 0`, which is the same test the wrapper makes.
	const bool cubeDirectTile = fds::FeatureFlags::deferred_cube_direct()
	    && !lmKernelEnabled
	    && !caFlags.profNoCubeTap
	    && caFlags.shadowMode == ShadowMode::PolyId;''')

sub(K,
'''			if (noncasterDepthG
			    && ((ctx.shadowSkipMask[matID >> 6] >> (matID & 63)) & 1u))
				surfaceShadowId = -1;''',
'''			if (noncasterDepthG
			    && ((ctx.shadowSkipMask[matID >> 6] >> (matID & 63)) & 1u))
				surfaceShadowId = -1;
			// --deferred_cube_direct (see cubeDirectTile).
			const bool cubeDirect = cubeDirectTile && surfaceShadowId >= 0;''')

sub(K,
'''						const int32_t cubeIdx = tl.cubeShadowIdx[n];
						if (cubeIdx >= 0) {
							const float cubeAtten = resolveCubeAtten(
								pixelLM, cubeIdx, lmKernelEnabled, caFlags,
								wx, wy, wz, lenInv,
								nGeoX, nGeoY, nGeoZ,
								sampleWorldX, sampleWorldY, sampleWorldZ,
								x, y, z, kShadowBiasG, kSlopeBiasG,
								surfaceShadowId);''',
'''						const int32_t cubeIdx = tl.cubeShadowIdx[n];
						if (cubeIdx >= 0) {
							// --deferred_cube_direct: this IS resolveCubeAtten's
							// PolyId arm, called with the arguments it forwards,
							// without the 20-argument frame around it.
							const float cubeAtten = cubeDirect
							    ? CubeShadow_Sample(cubeIdx,
								sampleWorldX, sampleWorldY, sampleWorldZ,
								x, y, z, /*constBias=*/0, /*slopeBias=*/0,
								surfaceShadowId)
							    : resolveCubeAtten(
								pixelLM, cubeIdx, lmKernelEnabled, caFlags,
								wx, wy, wz, lenInv,
								nGeoX, nGeoY, nGeoZ,
								sampleWorldX, sampleWorldY, sampleWorldZ,
								x, y, z, kShadowBiasG, kSlopeBiasG,
								surfaceShadowId);''')

# ── 5. the wave-2 fill: dead LDR divides under HDR + the per-pixel flag read ─
sub(K,
'''	const float quarterZJump  = (quarter || checker)
	    ? fds::FeatureFlags::quarter_z_jump() : 0.0f;
	const bool  quarterZCheck = (quarter || checker) && quarterZJump > 0.0f;''',
'''	const float quarterZJump  = (quarter || checker)
	    ? fds::FeatureFlags::quarter_z_jump() : 0.0f;
	const bool  quarterZCheck = (quarter || checker) && quarterZJump > 0.0f;
	// --deferred_fill_hdr_skip: hoist --quarter_tex_sharp (it was read once per
	// FILLED PIXEL) and gate the LDR sharp-reconstruction accumulators, which
	// are three divisions per compatible neighbour that only the !hdrWrite arm
	// below ever reads.
	const bool sTexSharp   = fds::FeatureFlags::quarter_tex_sharp();
	const bool fillLdrSharp = !(fds::FeatureFlags::deferred_fill_hdr_skip() && hdrWrite);''')

sub(K,
"""			const bool sTexSharp = fds::FeatureFlags::quarter_tex_sharp();

""", "")

for _tail in ["""							if (nh) {
								// radiance \u221d texel^exp (2 = hdr_linear albedo\u00b2, 1 = gamma);
								// re-apply own texel: R_i = R_n\u00b7(texel_i/texel_n)^exp.""",
              """							if (nh) {
								// radiance \u221d texel^exp; re-apply own texel (see quarter path)"""]:
    _old = """						    (nrR = ownR / std::max(nr, 1.0f)) <= 4.0f) {
							slB += float(p & 0xFF)        * 256.0f / std::max(nb, 1.0f);
							slG += float((p >> 8) & 0xFF)  * 256.0f / std::max(ng, 1.0f);
							slR += float((p >> 16) & 0xFF) * 256.0f / std::max(nr, 1.0f);
""" + _tail
    _new = """						    (nrR = ownR / std::max(nr, 1.0f)) <= 4.0f) {
							// --deferred_fill_hdr_skip: these three DIVISIONS feed
							// only the `!hdrWrite` arm below.
							if (fillLdrSharp) {
							slB += float(p & 0xFF)        * 256.0f / std::max(nb, 1.0f);
							slG += float((p >> 8) & 0xFF)  * 256.0f / std::max(ng, 1.0f);
							slR += float((p >> 16) & 0xFF) * 256.0f / std::max(nr, 1.0f);
							}
""" + _tail
    sub(K, _old, _new)

print("patch1 applied")
