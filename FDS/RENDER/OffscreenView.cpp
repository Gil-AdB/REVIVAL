#include "OffscreenView.h"

#include <Base/FDS_VARS.H>  // MainSurf, View, FOVX/FOVY, AspectRatio
#include <Base/FDS_DECS.H>  // VESA_Surface2Global, SetCurrentScene
#include <Base/Scene.h>

// Owned here (the natural home: this is the object whose job is the
// MainSurf swap). DEMO/Resize.cpp's resize path and CITY's cube bake
// reference it extern.
std::mutex g_engineSurfaceMutex;

namespace fds {

OffscreenViewScope::OffscreenViewScope(Scene *sc, VESA_Surface *target)
    : lk_(g_engineSurfaceMutex),
      sc_(sc),
      prevMain_(MainSurf),
      prevView_(::View),
      prevFOVX_(FOVX), prevFOVY_(FOVY),
      prevNZP_(sc->NZP), prevFZP_(sc->FZP),
      prevAspect_(AspectRatio)
{
    MainSurf = target;
}

void OffscreenViewScope::publishSurface()
{
    VESA_Surface2Global(MainSurf);
}

void OffscreenViewScope::setView(Camera *cam)
{
    ::View = cam;
}

void OffscreenViewScope::setNearZ(float nzp)
{
    sc_->NZP = nzp;
    SetCurrentScene(sc_);
}

void OffscreenViewScope::setFarZ(float fzp)
{
    sc_->FZP = fzp;
    SetCurrentScene(sc_);
}

OffscreenViewScope::~OffscreenViewScope()
{
    ::View = prevView_;
    sc_->NZP = prevNZP_;
    sc_->FZP = prevFZP_;
    AspectRatio = prevAspect_;
    MainSurf = prevMain_;
    VESA_Surface2Global(MainSurf);  // XRes/VPage/Cntr*/YOffs back
    SetCurrentScene(sc_);           // C_NZP/C_FZP/zscale/clipper back
    FOVX = prevFOVX_;               // not covered by Surface2Global;
    FOVY = prevFOVY_;               // nothing recomputes these until
                                    // the next CalcPersp.
}

}  // namespace fds
