#include "SpotlightCones.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>
#include <Base/Omni.h>
#include <Base/Spline.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace fds {

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
