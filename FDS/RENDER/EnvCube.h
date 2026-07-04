#ifndef FDS_ENV_CUBE_H_INCLUDED
#define FDS_ENV_CUBE_H_INCLUDED

// ─── Padded cube-face environment map: the one canonical convention ───────
//
// The equirect env path (EnvBake stitch, DeferredSurfaceKernel equirect
// lookup, Transform.cpp reflective vertex pass, CITY panorama table) is being
// replaced by six PADDED cube faces sampled with a TRIG-FREE lookup. This
// header is the single home for the convention so the equirect era's
// orientation drift (CITY's ROT quirk, ENVFLIP hacks, stitcher handedness)
// does not repeat: EVERY consumer (bake, deferred kernel, forward Transform,
// CITY bake, debug painters) uses these helpers.
//
// FACE ORDER / BASIS  (identical to EnvBake.cpp's kCubeFaces so the existing,
// proven cube-face renders map straight in):
//     0:+X  1:-X  2:+Y  3:-Y  4:+Z  5:-Z
// Each face has an orthonormal (right, up, fwd) basis; fwd is the outward view
// direction. The engine reads Camera::Mat rows as [right; up; fwd], so a bake
// camera built from EnvCubeBasis[k] renders exactly the image EnvCube_*ToDir
// samples. Screen-y grows DOWNWARD, hence the v = 0.5 - 0.5*b flip (matches
// EnvBake's sampleCube sy = (1-b) mapping).
//
// PADDING (D2): faces are baked with FOV > 90° so the nominal cube [-1,1]
// tangent range occupies the central 1/kEnvCubePad of the texture and the
// outer ring holds neighbour-face content. kEnvCubeInvPad folds into the
// lookup as one extra multiply. Padding absorbs (a) blur-chain neighbour
// reads at face edges, (b) per-triangle face-spanning in the forward path
// (UVs beyond ±1 land in valid padding, clamp beyond).
//
// LOOKUP COST (D1): 2-3 compares + selects, ONE divide (1/m), two fma. No
// atan/asin, ever. Strictly cheaper than the equirect polynomial approx.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace fds {

// Overscan factor: nominal [-1,1] maps to central 1/pad of the face.
constexpr float kEnvCubePad    = 1.25f;
constexpr float kEnvCubeInvPad = 1.0f / kEnvCubePad;   // 0.8
constexpr int   kEnvCubeFaces  = 6;

struct EnvCubeBasisT { float right[3], up[3], fwd[3]; };

// Basis per face — copied verbatim from EnvBake.cpp::kCubeFaces.
inline const EnvCubeBasisT& EnvCube_Basis(int face) {
    static const EnvCubeBasisT b[6] = {
        {{ 0, 0,-1}, {0, 1, 0}, { 1, 0, 0}},   // 0 +X
        {{ 0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},   // 1 -X
        {{ 1, 0, 0}, {0, 0,-1}, { 0, 1, 0}},   // 2 +Y
        {{ 1, 0, 0}, {0, 0, 1}, { 0,-1, 0}},   // 3 -Y
        {{ 1, 0, 0}, {0, 1, 0}, { 0, 0, 1}},   // 4 +Z
        {{-1, 0, 0}, {0, 1, 0}, { 0, 0,-1}},   // 5 -Z
    };
    return b[face];
}

// Full FOV (degrees) a bake camera must use so its square image spans the
// padded [-pad,pad] tangent range: half-angle = atan(pad).
inline float EnvCube_FaceFovDegrees() {
    return 2.0f * std::atan(kEnvCubePad) * (180.0f / 3.14159265358979323846f);
}

// dir → face + UV in [0,1). TRIG-FREE hot path. Normalization of `dir` is
// optional (any positive scale cancels in a/m). u,v are clamped to [0,1):
// values that would exceed the padded range (a triangle overhanging the face
// by more than the pad margin) clamp to the edge texel.
inline void EnvCube_DirToFaceUV(float dx, float dy, float dz,
                                int& face, float& u, float& v) {
    const float ax = std::fabs(dx), ay = std::fabs(dy), az = std::fabs(dz);
    float m, a, b;
    if (ax >= ay && ax >= az) {
        if (dx > 0.0f) { face = 0; m = ax; a = -dz; b =  dy; }
        else           { face = 1; m = ax; a =  dz; b =  dy; }
    } else if (ay >= az) {
        if (dy > 0.0f) { face = 2; m = ay; a =  dx; b = -dz; }
        else           { face = 3; m = ay; a =  dx; b =  dz; }
    } else {
        if (dz > 0.0f) { face = 4; m = az; a =  dx; b =  dy; }
        else           { face = 5; m = az; a = -dx; b =  dy; }
    }
    const float inv = (m > 1e-20f) ? (1.0f / m) : 0.0f;   // the ONE divide
    u = 0.5f + (0.5f * kEnvCubeInvPad) * (a * inv);
    v = 0.5f - (0.5f * kEnvCubeInvPad) * (b * inv);
    if (u < 0.0f) u = 0.0f; else if (u > 0.99999994f) u = 0.99999994f;
    if (v < 0.0f) v = 0.0f; else if (v > 0.99999994f) v = 0.99999994f;
}

// Which face a direction's dominant axis selects (0..5). Trig-free. Used by
// the forward path to pick ONE face per TRIANGLE from the centroid direction.
inline int EnvCube_SelectFace(float dx, float dy, float dz) {
    const float ax = std::fabs(dx), ay = std::fabs(dy), az = std::fabs(dz);
    if (ax >= ay && ax >= az) return dx > 0.0f ? 0 : 1;
    if (ay >= az)             return dy > 0.0f ? 2 : 3;
    return dz > 0.0f ? 4 : 5;
}

// Project a direction onto a SPECIFIC face's plane (gnomonic + pad), even if
// that face isn't the direction's dominant axis. The forward path selects the
// face once per triangle (from the centroid) then projects all three vertex
// dirs onto THAT face: dirs straddling into the padded ring land at u,v beyond
// the nominal ±1 window (still valid content), clamp beyond. u,v in [0,1).
//
// Returns false when the direction has NO valid projection on this face:
// backfacing (m ≤ 0 — gnomonic projection through the center, u/v would be
// the antipode) or outside the padded window (|tangent| > pad — the clamp
// would flat-line the UV and smear interpolation across the triangle). The
// caller must then take the wide-span fallback for the WHOLE triangle; the
// u,v written in that case are the clamped best-effort (diagnostic only).
// The original slice-C cut returned void and mapped backfacing dirs to the
// face CENTER (inv=0 → 0.5,0.5) — on close-up city towers, whose reflected
// dirs routinely exceed 90° off the centroid face, every such vertex
// collapsed to one texel and entire facades smeared toward it (the
// "wrong mapping, changes when moving" bug).
inline bool EnvCube_DirToUVOnFace(int face, float dx, float dy, float dz,
                                  float& u, float& v) {
    const EnvCubeBasisT& B = EnvCube_Basis(face);
    const float a = dx * B.right[0] + dy * B.right[1] + dz * B.right[2];
    const float b = dx * B.up[0]    + dy * B.up[1]    + dz * B.up[2];
    const float m = dx * B.fwd[0]   + dy * B.fwd[1]   + dz * B.fwd[2];
    if (m <= 1e-20f) { u = 0.5f; v = 0.5f; return false; }
    const float inv = 1.0f / m;
    const float ta = a * inv, tb = b * inv;
    u = 0.5f + (0.5f * kEnvCubeInvPad) * ta;
    v = 0.5f - (0.5f * kEnvCubeInvPad) * tb;
    if (u < 0.0f) u = 0.0f; else if (u > 0.99999994f) u = 0.99999994f;
    if (v < 0.0f) v = 0.0f; else if (v > 0.99999994f) v = 0.99999994f;
    return std::fabs(ta) <= kEnvCubePad && std::fabs(tb) <= kEnvCubePad;
}

// face + UV in [0,1] → unit direction. Exact algebraic inverse of
// DirToFaceUV within the padded window; used by the bake (per-texel inverse)
// and diagnostics. In the padded ring / overhang region the returned dir may
// belong to a NEIGHBOUR face under DirToFaceUV — that is the padding working
// as designed, not an error.
inline void EnvCube_FaceUVToDir(int face, float u, float v,
                                float& dx, float& dy, float& dz) {
    const float a = (u - 0.5f) * (2.0f * kEnvCubePad);
    const float b = (0.5f - v) * (2.0f * kEnvCubePad);
    const EnvCubeBasisT& B = EnvCube_Basis(face);
    float x = B.fwd[0] + a * B.right[0] + b * B.up[0];
    float y = B.fwd[1] + a * B.right[1] + b * B.up[1];
    float z = B.fwd[2] + a * B.right[2] + b * B.up[2];
    const float inv = 1.0f / std::sqrt(x * x + y * y + z * z);
    dx = x * inv; dy = y * inv; dz = z * inv;
}

// Round-trip validation. dir → face/uv → dir angular error over a dense
// sphere sample, plus a central-window uv → dir → face/uv check. Prints a
// summary; returns false (and the caller should abort) on gross failure.
// Runs under FDS_ENV_CUBE_SELFTEST — see the REV.CPP startup hook.
inline bool EnvCube_SelfTest(bool verbose = true) {
    double maxAngErr = 0.0;      // dir → uv → dir
    double maxUvErr  = 0.0;      // central uv → dir → uv
    int    faceMiss  = 0;
    const int N = 256;
    for (int i = 0; i <= N; ++i) {
        for (int j = 0; j <= N; ++j) {
            // Fibonacci-ish spherical sweep via a lat/long grid.
            const float lon = (float(i) / N) * 6.2831853f;
            const float lat = (float(j) / N - 0.5f) * 3.1415927f;
            const float cl = std::cos(lat);
            float dx = std::cos(lon) * cl, dy = std::sin(lat), dz = std::sin(lon) * cl;
            int f; float u, vv;
            EnvCube_DirToFaceUV(dx, dy, dz, f, u, vv);
            float rx, ry, rz;
            EnvCube_FaceUVToDir(f, u, vv, rx, ry, rz);
            float d = dx * rx + dy * ry + dz * rz;
            if (d > 1.0f) d = 1.0f; if (d < -1.0f) d = -1.0f;
            const double ang = std::acos(d);
            if (ang > maxAngErr) maxAngErr = ang;
        }
    }
    // Central-window uv round-trip: u,v in [0.15,0.85] stay within |a|,|b|<=1
    // so the same face wins on the way back.
    for (int f = 0; f < 6; ++f) {
        for (int i = 0; i <= 32; ++i) {
            for (int j = 0; j <= 32; ++j) {
                const float u = 0.15f + (0.70f * i) / 32.0f;
                const float vv = 0.15f + (0.70f * j) / 32.0f;
                float dx, dy, dz; EnvCube_FaceUVToDir(f, u, vv, dx, dy, dz);
                int f2; float u2, v2; EnvCube_DirToFaceUV(dx, dy, dz, f2, u2, v2);
                if (f2 != f) { ++faceMiss; continue; }
                const double e = std::fabs(u2 - u) + std::fabs(v2 - vv);
                if (e > maxUvErr) maxUvErr = e;
            }
        }
    }
    const bool ok = (maxAngErr < 1e-3) && (maxUvErr < 1e-3) && (faceMiss == 0);
    if (verbose) {
        std::fprintf(stderr,
            "[ENVCUBE-SELFTEST] pad=%.3f fov=%.2fdeg  dir->uv->dir maxAngErr=%.3e rad  "
            "central uv->dir->uv maxErr=%.3e  faceMiss=%d  => %s\n",
            kEnvCubePad, EnvCube_FaceFovDegrees(), maxAngErr, maxUvErr, faceMiss,
            ok ? "PASS" : "FAIL");
    }
    return ok;
}

// Paint one debug face into a res*res ARGB (0xAARRGGBB) buffer: a per-face
// tint, an 8×8 grid, a yellow rectangle at the nominal ±1 window boundary
// (so the padding ring is visible), origin/axis markers (green block at the
// (u,v)=(0,0) corner, red bar along +u, blue bar along +v), and `face+1`
// white dots. The cube analog of CITY's paintDebugPanorama — bake it and
// verify each face lands where its markers say. No font needed.
inline void EnvCube_PaintDebugFace(int face, uint32_t* px, int res) {
    static const uint32_t kTint[6] = {
        0xFF803030u, 0xFF304030u, 0xFF303080u,
        0xFF806030u, 0xFF308060u, 0xFF603080u,
    };
    const uint32_t tint = kTint[face % 6];
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            uint32_t c = tint;
            // 8×8 grid.
            if ((x % (res / 8)) == 0 || (y % (res / 8)) == 0) c = 0xFF404040u;
            px[size_t(y) * res + x] = c;
        }
    // Nominal ±1 window rectangle (u,v in [0.1,0.9] for pad 1.25).
    const int lo = int(res * (0.5f - 0.5f * kEnvCubeInvPad));   // ~0.1*res
    const int hi = int(res * (0.5f + 0.5f * kEnvCubeInvPad));   // ~0.9*res
    for (int t = lo; t <= hi && t < res; ++t) {
        if (lo >= 0 && lo < res) { px[size_t(lo) * res + t] = 0xFFFFFF00u; px[size_t(t) * res + lo] = 0xFFFFFF00u; }
        if (hi >= 0 && hi < res) { px[size_t(hi) * res + t] = 0xFFFFFF00u; px[size_t(t) * res + hi] = 0xFFFFFF00u; }
    }
    // Origin corner (u,v)=(0,0) = top-left: green block.
    const int mk = res / 10;
    for (int y = 0; y < mk; ++y)
        for (int x = 0; x < mk; ++x) px[size_t(y) * res + x] = 0xFF00FF00u;
    // +u bar along the top edge (red), +v bar down the left edge (blue).
    for (int x = 0; x < res; ++x)      px[size_t(2) * res + x] = 0xFFFF0000u;
    for (int y = 0; y < res; ++y)      px[size_t(y) * res + 2] = 0xFF0000FFu;
    // face+1 white dots along the center row.
    const int dr = res / 2, dd = res / 12;
    for (int k = 0; k <= face; ++k) {
        const int cx = res / 2 + (k - face) * (dd * 2) + dd;
        for (int y = -dd / 2; y <= dd / 2; ++y)
            for (int x = -dd / 2; x <= dd / 2; ++x) {
                const int xx = cx + x, yy = dr + y;
                if (xx >= 0 && xx < res && yy >= 0 && yy < res)
                    px[size_t(yy) * res + xx] = 0xFFFFFFFFu;
            }
    }
}

}  // namespace fds

#endif  // FDS_ENV_CUBE_H_INCLUDED
