#ifndef REVIVAL_STATIC_SHADOW_LIGHTMAP_H
#define REVIVAL_STATIC_SHADOW_LIGHTMAP_H

#include "BaseDefs.h"

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
};

#endif // REVIVAL_STATIC_SHADOW_LIGHTMAP_H
