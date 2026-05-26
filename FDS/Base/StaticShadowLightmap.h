#ifndef REVIVAL_STATIC_SHADOW_LIGHTMAP_H
#define REVIVAL_STATIC_SHADOW_LIGHTMAP_H

#include "BaseDefs.h"

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>

// Per-face mini-atlas storing pre-baked shadow factors from each static
// omni. Hangs off TriMesh::staticShadowLM (nullptr for dynamic meshes or
// when --shadow-lightmap is off). See docs/STATIC_SHADOW_LIGHTMAPS.md
// for the design rationale (Option A: per-face atlas indexed by
// barycentric (s, t), no UV channel needed, no FLD changes).
//
// Memory layout: data[face][y * N + x][omni]  — omnis stored adjacent so
// the deferred kernel reads K consecutive bytes per pixel (one cache
// line for K ≤ 64).
//
// Pack-state hardening: TriMesh.h is included in a `#pragma pack(push, 1)`
// region, and some other headers in the tree leave pack(1) active in
// subtly-transitive ways. We force default alignment here so the std::
// vector members keep their natural 8-byte alignment regardless of
// caller include order — otherwise different TUs see different struct
// sizes (84 vs 88 bytes), the `data` vector's metadata lives at a
// different offset, and the lighting kernel reads zeros where the
// bake wrote 14 MB of texels.
#pragma pack(push, 8)
struct StaticShadowLightmap {
    int lmRes    = 16;   // N per-face edge length (default 16 → 16×16 atlas per face)
    int numFaces = 0;    // M, matches the mesh's face count at bake time
    int numOmnis = 0;    // K, # static-omni shadow casters baked for this mesh

    // Per-(face, omni) "any non-trivially-lit texel?" coverage bit. Set
    // by the bake whenever a texel for this (face, omni) pair came out
    // > 0 after the per-texel cull (range + back-face). Lets the
    // deferred kernel early-skip omnis that contribute nothing here.
    // Indexed bit position: face * K + lmOmniIdx.
    std::vector<uint8_t> coverageBits;

    // Maps lightmap-local omni index ∈ [0, K) → scene-wide static-omni
    // index that bake-time saw. Used at runtime to correlate against
    // ViewLightsSoA entries (kernel finds lmIdx from its per-frame omni
    // sequence). -1 sentinel until set by the baker.
    std::vector<int> omniSceneIdx;

    // The shadow bytes: M * N * N * K. 0 = fully shadowed, 255 = fully lit.
    std::vector<uint8_t> data;

    // Compute the (face, x, y) → byte-array-of-K-omnis pointer. Bounds-
    // checking is the caller's responsibility (no asserts in the hot path).
    inline const uint8_t* texel(int face, int x, int y) const {
        const size_t faceStride = size_t(lmRes) * size_t(lmRes) * size_t(numOmnis);
        const size_t pxOff      = size_t(y * lmRes + x) * size_t(numOmnis);
        return data.data() + size_t(face) * faceStride + pxOff;
    }
    inline uint8_t* texel(int face, int x, int y) {
        const size_t faceStride = size_t(lmRes) * size_t(lmRes) * size_t(numOmnis);
        const size_t pxOff      = size_t(y * lmRes + x) * size_t(numOmnis);
        return data.data() + size_t(face) * faceStride + pxOff;
    }

    inline bool covers(int face, int lmOmniIdx) const {
        const size_t bit = size_t(face) * size_t(numOmnis) + size_t(lmOmniIdx);
        return (coverageBits[bit >> 3] >> (bit & 7)) & 1u;
    }
    inline void setCovers(int face, int lmOmniIdx) {
        const size_t bit = size_t(face) * size_t(numOmnis) + size_t(lmOmniIdx);
        coverageBits[bit >> 3] |= uint8_t(1u << (bit & 7));
    }

    // Sized allocation. Re-callable if scene contents change; existing
    // data is dropped. `omniSceneIdx` is left -1 — baker fills it.
    void allocate(int faces, int omnis, int res = 16) {
        lmRes    = res;
        numFaces = faces;
        numOmnis = omnis;
        data.assign(size_t(faces) * size_t(res) * size_t(res) * size_t(omnis), 0);
        coverageBits.assign((size_t(faces) * size_t(omnis) + 7u) / 8u, 0);
        omniSceneIdx.assign(size_t(omnis), -1);
    }

    void clear() {
        data.clear();
        coverageBits.clear();
        omniSceneIdx.clear();
        numFaces = numOmnis = 0;
    }

    // Bilinear sample at fractional texel coord. Input is the G-buffer-
    // encoded barycentric pair (s, t) as 8-bit fractions in [0, 255]. The
    // omni index is the position in this lightmap's omni table (which the
    // baker initializes 1:1 with g_cubeShadowRefs, so the lighting kernel
    // passes its cubeIdx directly). Returns shadow factor in [0, 1]:
    // 1.0 = fully lit, 0.0 = fully shadowed.
    inline float sampleBilinear(int faceIdx, int omniIdx,
                                uint8_t sB, uint8_t tB) const {
        if (data.empty() || lmRes < 2 || numOmnis <= 0 ||
            faceIdx < 0 || faceIdx >= numFaces ||
            omniIdx < 0 || omniIdx >= numOmnis) {
            return 1.0f;
        }
        const float gridMax = float(lmRes - 1);
        const float sf = (float(sB) * (1.0f / 255.0f)) * gridMax;
        const float tf = (float(tB) * (1.0f / 255.0f)) * gridMax;
        int tx = int(sf), ty = int(tf);
        if (tx < 0) tx = 0; if (tx > lmRes - 2) tx = lmRes - 2;
        if (ty < 0) ty = 0; if (ty > lmRes - 2) ty = lmRes - 2;
        const float fx = sf - float(tx);
        const float fy = tf - float(ty);
        const uint8_t *p00 = texel(faceIdx, tx,     ty)     + omniIdx;
        const uint8_t *p10 = texel(faceIdx, tx + 1, ty)     + omniIdx;
        const uint8_t *p01 = texel(faceIdx, tx,     ty + 1) + omniIdx;
        const uint8_t *p11 = texel(faceIdx, tx + 1, ty + 1) + omniIdx;
        const float v00 = float(*p00);
        const float v10 = float(*p10);
        const float v01 = float(*p01);
        const float v11 = float(*p11);
        const float v0 = v00 + (v10 - v00) * fx;
        const float v1 = v01 + (v11 - v01) * fx;
        return (v0 + (v1 - v0) * fy) * (1.0f / 255.0f);
    }

    // Nearest-neighbor sample at fractional texel coord. Same indexing
    // as sampleBilinear but picks the closest texel, no blend across
    // neighbors. Used by --shadow-lightmap-nearest to isolate whether
    // visible seam artifacts come from bilinear's cross-edge blend (then
    // nearest fixes them, just blockier) or from the atlas itself (then
    // nearest reproduces the same artifacts).
    inline float sampleNearest(int faceIdx, int omniIdx,
                               uint8_t sB, uint8_t tB) const {
        if (data.empty() || lmRes < 1 || numOmnis <= 0 ||
            faceIdx < 0 || faceIdx >= numFaces ||
            omniIdx < 0 || omniIdx >= numOmnis) {
            return 1.0f;
        }
        const float gridMax = float(lmRes - 1);
        int tx = int((float(sB) * (1.0f / 255.0f)) * gridMax + 0.5f);
        int ty = int((float(tB) * (1.0f / 255.0f)) * gridMax + 0.5f);
        if (tx < 0) tx = 0; if (tx > lmRes - 1) tx = lmRes - 1;
        if (ty < 0) ty = 0; if (ty > lmRes - 1) ty = lmRes - 1;
        const uint8_t *p = texel(faceIdx, tx, ty) + omniIdx;
        return float(*p) * (1.0f / 255.0f);
    }
};
#pragma pack(pop)

#endif // REVIVAL_STATIC_SHADOW_LIGHTMAP_H
