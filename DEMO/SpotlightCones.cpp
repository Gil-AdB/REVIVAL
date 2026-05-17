#include "SpotlightCones.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Scene.h>
#include <Base/Omni.h>
#include <Base/Spline.h>
#include <Base/Camera.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace fds {

namespace {

struct ConeVtx { float vx, vy, vz; float alpha; };

// Per-frame tuning, cached at first render call. Cone params change
// only when the user flips a CLI flag, so once-per-process is fine.
struct ConeParams {
    float strength;       // apex alpha multiplier
    float falloffExp;     // alpha = bary^exp (1=flat, 2=quadratic, 3=cubic)
    bool  distFalloff;    // attenuate intensity with view distance
};
inline ConeParams loadParams() {
    return {
        fds::FeatureFlags::cone_strength(),
        fds::FeatureFlags::cone_falloff_exp(),
        fds::FeatureFlags::cone_dist_falloff() != 0,
    };
}

inline Vector worldToView(const Vector& world) {
    const Vector d = {
        world.x - View->ISource.x,
        world.y - View->ISource.y,
        world.z - View->ISource.z };
    Vector v;
    MatrixXVector(View->Mat, &d, &v);
    return v;
}

// Project view-space (vx,vy,vz) to screen. Near-plane-clipped verts
// have vz ≈ NZP so projected x/y can be huge in float — the bbox
// clamp at the rasterizer handles it. Don't clamp x/y here, that
// produces phantom triangles.
inline void projectViewVtx(float vx, float vy, float vz,
                            float &outX, float &outY) {
    const float invZ = 1.0f / vz;
    outX = CntrX + FOVX * vx * invZ;
    outY = CntrY - FOVY * vy * invZ;
}

// Sutherland-Hodgman near-plane clip — 1→3 in, ≤4 out.
inline int clipTriNear(const ConeVtx in[3], ConeVtx out[4], float nearZ) {
    int outCount = 0;
    ConeVtx prev = in[2];
    bool prevIn = prev.vz > nearZ;
    for (int i = 0; i < 3; ++i) {
        const ConeVtx &cur = in[i];
        const bool curIn = cur.vz > nearZ;
        if (prevIn != curIn) {
            const float t = (nearZ - prev.vz) / (cur.vz - prev.vz);
            out[outCount].vx    = prev.vx    + t * (cur.vx    - prev.vx);
            out[outCount].vy    = prev.vy    + t * (cur.vy    - prev.vy);
            out[outCount].vz    = nearZ;
            out[outCount].alpha = prev.alpha + t * (cur.alpha - prev.alpha);
            ++outCount;
        }
        if (curIn) out[outCount++] = cur;
        prev = cur;
        prevIn = curIn;
    }
    return outCount;
}

// Per-triangle additive fill with depth-tested perspective-correct Z
// interpolation + alpha falloff. View-z lerps as 1/z (linear in
// screen-space under perspective). Two-sided (sign of area and w_i
// match, so w_i>=0 test stays correct).
//
// distFalloff: when true, multiply alpha by (1 - clamp(pz/maxDist)).
inline void addTri(float x0, float y0, float z0, float a0,
                   float x1, float y1, float z1, float a1,
                   float x2, float y2, float z2, float a2,
                   uint8_t cr, uint8_t cg, uint8_t cb,
                   const ConeParams& p, float maxFadeDist) {
    if (z0 <= 0.0f || z1 <= 0.0f || z2 <= 0.0f) return;
    const float area = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
    if (std::fabs(area) < 1.0f) return;
    const float invArea = 1.0f / area;
    const float rz0 = 1.0f / z0;
    const float rz1 = 1.0f / z1;
    const float rz2 = 1.0f / z2;
    float fxmin = std::min({x0,x1,x2});
    float fxmax = std::max({x0,x1,x2});
    float fymin = std::min({y0,y1,y2});
    float fymax = std::max({y0,y1,y2});
    if (fxmin < 0.0f)          fxmin = 0.0f;
    if (fymin < 0.0f)          fymin = 0.0f;
    if (fxmax > float(XRes-1)) fxmax = float(XRes-1);
    if (fymax > float(YRes-1)) fymax = float(YRes-1);
    const int xmin = int(fxmin);
    const int xmax = int(fxmax);
    const int ymin = int(fymin);
    const int ymax = int(fymax);
    if (xmin > xmax || ymin > ymax) return;
    dword *fb = (dword*)VPage;
    const word *zb = ZPage16;
    const float invMaxFade = p.distFalloff && maxFadeDist > 0.0f
                                 ? 1.0f / maxFadeDist
                                 : 0.0f;
    for (int y = ymin; y <= ymax; ++y) {
        const float fy = float(y);
        for (int x = xmin; x <= xmax; ++x) {
            const float fx = float(x);
            const float w0 = ((x1-fx)*(y2-fy) - (x2-fx)*(y1-fy)) * invArea;
            const float w1 = ((x2-fx)*(y0-fy) - (x0-fx)*(y2-fy)) * invArea;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            const float prz = w0*rz0 + w1*rz1 + w2*rz2;
            if (prz <= 0.0f) continue;
            const float pz = 1.0f / prz;
            int penc = 0xFF80 - int(pz * g_zscale);
            if (penc < 0)      penc = 0;
            if (penc > 0xFFFF) penc = 0xFFFF;
            const size_t i = size_t(y) * size_t(XRes) + size_t(x);
            const int zexist = int(zb[i]);
            if (penc < zexist) continue;
            float alpha = w0*a0 + w1*a1 + w2*a2;
            if (alpha <= 0.0f) continue;
            // Tunable falloff curve. exp=1 linear, exp=2 quadratic,
            // exp=3 cubic. Cone interior fills faster than rim with
            // higher exp = sharper hot core + cleaner rim.
            if (p.falloffExp == 2.0f) {
                alpha = alpha * alpha;
            } else if (p.falloffExp == 3.0f) {
                alpha = alpha * alpha * alpha;
            } else if (p.falloffExp != 1.0f) {
                alpha = std::pow(alpha, p.falloffExp);
            }
            // View-distance fade: fade out cones that are far away so
            // distant streetlights don't visually blob into a uniform
            // glow. Linear from 1.0 at z=0 down to 0.0 at maxFadeDist.
            if (invMaxFade > 0.0f) {
                float distAtten = 1.0f - pz * invMaxFade;
                if (distAtten < 0.0f) continue;
                alpha *= distAtten;
            }
            const int addR = int(float(cr) * alpha);
            const int addG = int(float(cg) * alpha);
            const int addB = int(float(cb) * alpha);
            const dword pix = fb[i];
            int oldR = int((pix >> 16) & 0xFF);
            int oldG = int((pix >>  8) & 0xFF);
            int oldB = int( pix        & 0xFF);
            int newR = oldR + addR; if (newR > 255) newR = 255;
            int newG = oldG + addG; if (newG > 255) newG = 255;
            int newB = oldB + addB; if (newB > 255) newB = 255;
            fb[i] = (dword(newR) << 16) | (dword(newG) << 8)
                    |  dword(newB)        | 0xFF000000u;
        }
    }
}

} // namespace

void SpotlightConeOverlay::render(Scene *sc) {
    if (!sc || !View) return;
    static const ConeParams params = loadParams();
    const float kNearZ = std::max(0.01f, sc->NZP);
    // Distance fade: cones invisible beyond maxFadeDist from camera.
    // Default to half the scene far plane — gives a natural city/atmos
    // feel where distant lights are dim, near lights are vivid.
    const float maxFadeDist = params.distFalloff ? sc->FZP * 0.5f : 0.0f;
    constexpr int N_SEGMENTS = 16;
    for (Omni *o = sc->OmniHead; o; o = o->Next) {
        if (o->Type != Light_SpotLight) continue;
        if (!(o->Flags & Omni_Active)) continue;
        const float cosOuter = std::max(0.01f, o->FallOff);
        const float outerAng = std::acos(cosOuter);
        Vector axis = o->IDir;
        Vector_Norm(&axis);
        Vector ref = (std::fabs(axis.y) < 0.9f)
                         ? Vector(0.0f, 1.0f, 0.0f)
                         : Vector(1.0f, 0.0f, 0.0f);
        Vector right;
        Cross_Product(&axis, &ref, &right);
        Vector_Norm(&right);
        Vector up;
        Cross_Product(&right, &axis, &up);
        Vector_Norm(&up);
        const float L = o->IRange;
        const float rimAxis = L * std::cos(outerAng);
        const float rimRad  = L * std::sin(outerAng);

        const Vector apexV = worldToView(o->IPos);
        ConeVtx rimV[N_SEGMENTS + 1];
        for (int s = 0; s <= N_SEGMENTS; ++s) {
            const float theta = float(s) * (2.0f * float(PI) / float(N_SEGMENTS));
            const float cs = std::cos(theta), sn = std::sin(theta);
            const Vector rim = {
                o->IPos.x + axis.x * rimAxis + (right.x * cs + up.x * sn) * rimRad,
                o->IPos.y + axis.y * rimAxis + (right.y * cs + up.y * sn) * rimRad,
                o->IPos.z + axis.z * rimAxis + (right.z * cs + up.z * sn) * rimRad,
            };
            const Vector rimVw = worldToView(rim);
            rimV[s].vx = rimVw.x;
            rimV[s].vy = rimVw.y;
            rimV[s].vz = rimVw.z;
            rimV[s].alpha = 0.0f;
        }

        // Per-spot intensity. Additive blending blows out white-ish
        // cones faster than saturated colors (R+G+B sum), so dim
        // achromatic cones by their chroma — pure white drops to
        // ~30% of color-channel intensity, saturated colors stay full.
        const float maxC = std::max({o->L.R, o->L.G, o->L.B});
        const float minC = std::min({o->L.R, o->L.G, o->L.B});
        const float chroma = (maxC > 1.0f) ? (maxC - minC) / maxC : 0.0f;
        const float whitenessScale = 0.3f + 0.7f * chroma;
        const float intensity = std::min(1.0f, o->ISize * 0.1f) * whitenessScale;
        const uint8_t cr = uint8_t(std::min(255.0f, o->L.R * intensity));
        const uint8_t cg = uint8_t(std::min(255.0f, o->L.G * intensity));
        const uint8_t cb = uint8_t(std::min(255.0f, o->L.B * intensity));

        const ConeVtx apexCV = { apexV.x, apexV.y, apexV.z, params.strength };
        for (int s = 0; s < N_SEGMENTS; ++s) {
            const ConeVtx tri[3] = { apexCV, rimV[s], rimV[s + 1] };
            ConeVtx clipped[4];
            const int n = clipTriNear(tri, clipped, kNearZ);
            if (n < 3) continue;
            float sx[4], sy[4];
            for (int k = 0; k < n; ++k) {
                projectViewVtx(clipped[k].vx, clipped[k].vy, clipped[k].vz,
                               sx[k], sy[k]);
            }
            for (int k = 2; k < n; ++k) {
                addTri(sx[0],   sy[0],   clipped[0].vz,   clipped[0].alpha,
                       sx[k-1], sy[k-1], clipped[k-1].vz, clipped[k-1].alpha,
                       sx[k],   sy[k],   clipped[k].vz,   clipped[k].alpha,
                       cr, cg, cb, params, maxFadeDist);
            }
        }
    }
}

// Animate_Objects walks each Omni's Pos/Size/Range splines and
// crashes on NumKeys==0. Runtime-authored spots install a single
// constant key (Pos with the spot's IPos, Size/Range scalar values).
// SplineKey::Pos is a Quaternion not a Vector — copy per-component.
// Scenes that animate the spot per-frame (e.g. greets robot/orbit
// spots) can still overwrite IPos after Animate_Objects runs.
static void initSpotlightSingleKeySplines(Omni* o) {
    auto initSingleKey = [](Spline& sp, float x, float y, float z) {
        sp.NumKeys = 1;
        sp.Keys = new SplineKey;
        std::memset(sp.Keys, 0, sizeof(SplineKey));
        sp.Keys[0].Pos.x = x;
        sp.Keys[0].Pos.y = y;
        sp.Keys[0].Pos.z = z;
        sp.Flags = 0;
        sp.CurKey = 0;
    };
    initSingleKey(o->Pos,   o->IPos.x, o->IPos.y, o->IPos.z);
    initSingleKey(o->Size,  o->ISize,  o->ISize,  o->ISize);
    initSingleKey(o->Range, o->IRange, o->IRange, o->IRange);
}

Omni* MakeSpotLight(Scene* sc,
                     float R, float G, float B,
                     float intensity, float range,
                     const Vector& pos, const Vector& dir,
                     float hotInnerDeg, float fallOuterDeg,
                     uint16_t shadowMapRes,
                     bool castsShadow)
{
    Omni* o = (Omni*)getAlignedBlock(sizeof(Omni), 16);
    std::memset(o, 0, sizeof(Omni));
    o->L.R = R; o->L.G = G; o->L.B = B; o->L.A = 1.0f;
    o->ISize  = intensity;
    o->IRange = range;
    o->rRange = 1.0f / range;
    o->IPos   = pos;
    o->IDir   = dir;
    o->Type   = Light_SpotLight;
    o->HotSpot = std::cos(hotInnerDeg  * 3.14159f / 180.0f);
    o->FallOff = std::cos(fallOuterDeg * 3.14159f / 180.0f);
    o->Flags   = Omni_Active | (castsShadow ? Omni_CastsShadow : 0u);
    if (shadowMapRes) o->shadowMapRes = shadowMapRes;

    // Flare-pass plumbing: F.A/B/C must point at o->V (otherwise
    // Transform_Objects's flare pass derefs nulls); filler is a no-op
    // since cones don't render as flares.
    o->F.A = &o->V;
    o->F.B = &o->V;
    o->F.C = &o->V;
    o->F.Filler = [](Face*, Vertex**, dword, dword,
                     const fds::RenderTarget&,
                     const fds::CameraContext&) {};

    initSpotlightSingleKeySplines(o);

    // Prepend into the scene's doubly-linked omni chain.
    o->Next = sc->OmniHead;
    if (sc->OmniHead) sc->OmniHead->Prev = o;
    sc->OmniHead = o;
    return o;
}

} // namespace fds
