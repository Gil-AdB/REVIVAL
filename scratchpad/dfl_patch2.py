#!/usr/bin/env python3
"""Patch 2: --pom_horizon's per-pixel record behind one pointer. Byte-null;
no runtime hatch, because the values it stops materialising are unread."""
K = '/Users/gil-ad/work/rev-deflight/FDS/RENDER/DeferredSurfaceKernel.cpp'
F = '/Users/gil-ad/work/rev-deflight/FDS/Base/FeatureFlags.def'
def sub(path, old, new, n=1):
    s = open(path).read()
    assert s.count(old) == n, (path, s.count(old), old[:80])
    open(path, 'w').write(s.replace(old, new, n))

# ── 6. the --pom_horizon per-pixel record, when the feature is OFF ──────────
sub(K,
"""			const PomHorizonMap *hzMap =
				(pomHorizonOnG && !gb.tangent.empty()) ? Mat->PomHorizon : nullptr;
			const unsigned char *hzTexel = nullptr;
			float hzTx = 0, hzTy = 0, hzTz = 0, hzBx = 0, hzBy = 0, hzBz = 0;
			if (hzMap && miplevel < hzMap->numMipmaps && hzMap->data) {
				const meka::u16 packedT = gb.tangent[i];
				if (packedT != 0) {
					float tx, ty, tz;
					meka::oct_decode_u16(packedT, tx, ty, tz);
					const float tDotN = tx*nGeoX + ty*nGeoY + tz*nGeoZ;
					tx -= nGeoX * tDotN; ty -= nGeoY * tDotN; tz -= nGeoZ * tDotN;
					const float tLen2 = tx*tx + ty*ty + tz*tz;
					if (tLen2 > 1e-12f) {
						const float inv = fast_rsqrt(tLen2);
						hzTx = tx*inv; hzTy = ty*inv; hzTz = tz*inv;
						const float hs = Mat->TbnHandedness;
						hzBx = (nGeoY*hzTz - nGeoZ*hzTy) * hs;
						hzBy = (nGeoZ*hzTx - nGeoX*hzTz) * hs;
						hzBz = (nGeoX*hzTy - nGeoY*hzTx) * hs;
						hzTexel = hzMap->data
						        + (hzMap->mipOfs[miplevel] + swizzledUV) * kPomHorizonAzimuths;
					}
				}
			}""",
"""			// The relief-horizon record, behind ONE pointer instead of seven
			// per-pixel locals. It used to resolve Mat->PomHorizon and set six
			// tangent-frame floats plus a texel pointer for EVERY shaded pixel,
			// and all seven then stayed live across the whole per-light loop in
			// a body the register allocator is already spilling. MEASURED by the
			// per-pixel ablation ladder at greets t=5743: 0.034 Gi/f — 1.7 % of
			// the entire DeferredLighting call — for a flag (--pom_horizon) that
			// is OFF in every shipping scene. No hatch: nothing reads what this
			// stops materialising, so there is no arm to compare.
			HzFrame hzF;
			const HzFrame *hzRec = nullptr;
			if (pomHorizonOnG && !gb.tangent.empty()) {
				const PomHorizonMap *hzMap = Mat->PomHorizon;
				if (hzMap && miplevel < hzMap->numMipmaps && hzMap->data) {
					const meka::u16 packedT = gb.tangent[i];
					if (packedT != 0) {
						float tx, ty, tz;
						meka::oct_decode_u16(packedT, tx, ty, tz);
						const float tDotN = tx*nGeoX + ty*nGeoY + tz*nGeoZ;
						tx -= nGeoX * tDotN; ty -= nGeoY * tDotN; tz -= nGeoZ * tDotN;
						const float tLen2 = tx*tx + ty*ty + tz*tz;
						if (tLen2 > 1e-12f) {
							const float inv = fast_rsqrt(tLen2);
							hzF.tx = tx*inv; hzF.ty = ty*inv; hzF.tz = tz*inv;
							const float hs = Mat->TbnHandedness;
							hzF.bx = (nGeoY*hzF.tz - nGeoZ*hzF.ty) * hs;
							hzF.by = (nGeoZ*hzF.tx - nGeoX*hzF.tz) * hs;
							hzF.bz = (nGeoX*hzF.ty - nGeoY*hzF.tx) * hs;
							hzF.texel = hzMap->data
							          + (hzMap->mipOfs[miplevel] + swizzledUV) * kPomHorizonAzimuths;
							hzRec = &hzF;
						}
					}
				}
			}""")

sub(K,
"""						if (hzTexel) {
							const float lx = wx * lenInv, ly = wy * lenInv, lzv = wz * lenInv;""",
"""						if (hzRec) {
							const float lx = wx * lenInv, ly = wy * lenInv, lzv = wz * lenInv;
							const float hzTx = hzRec->tx, hzTy = hzRec->ty, hzTz = hzRec->tz;
							const float hzBx = hzRec->bx, hzBy = hzRec->by, hzBz = hzRec->bz;
							const unsigned char *const hzTexel = hzRec->texel;""")

sub(K,
"""			if (pomHorizonVizG && hzTexel) {""",
"""			if (pomHorizonVizG && hzRec) {""")

# the struct type, file scope
sub(K,
"""static void Render_DeferredLighting_Tile(const DeferredLightingCtx &ctx,""",
"""// --pom_horizon's per-pixel relief record, gathered so the pixel body carries
// ONE pointer instead of seven locals across the per-light loop.
struct HzFrame { const unsigned char *texel; float tx, ty, tz, bx, by, bz; };

static void Render_DeferredLighting_Tile(const DeferredLightingCtx &ctx,""")

print("horizon patched")
