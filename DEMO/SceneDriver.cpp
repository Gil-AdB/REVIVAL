#include "SceneTick.h"
#include <Base/ParamScript.h>

#include "Rev.h"

#include <Base/FDS_DECS.H>
#include <Base/FDS_DEFS.H>
#include <Base/FDS_VARS.H>
#include <Base/Scene.h>
#include <Base/Face.h>
#include <Base/TriMesh.h>
#include <Base/Omni.h>
#include <Base/Camera.h>
#include <Base/Material.h>
#include <Threads.h>

#include <cstdio>
#include <cstring>

void SceneDriver::setupFaceLists(Scene *sc, bool includeOmnisInCount)
{
    int32_t polys = 0;
    for (TriMesh *t = sc->TriMeshHead; t; t = t->Next)
        polys += t->FIndex;
    if (includeOmnisInCount) {
        for (Omni *o = sc->OmniHead; o; o = o->Next) {
            (void)o;
            ++polys;
        }
    }
    // Particles insert into the same FList: 1 face each, or 2 for trail
    // quads (addParticleTrail). Scenes that allocate Sc->Pcl before
    // driver init (city rain) need the headroom; fountain sizes its own
    // lists and doesn't come through here.
    polys += sc->NumOfParticles * 2;
    fds::g_mainFaces.resize(polys);
    View  = sc->CameraHead;
    C_FZP  = sc->FZP;
    C_rFZP = 1.0f / C_FZP;
}

void SceneDriver::tickTabToggle(Scene *sc, const char *sceneName)
{
    // Per-scene scripted parameters ride this hook: every scene already
    // calls tickTabToggle(sc, "<name>") once per frame after its Timer
    // update, which is exactly the (scene name, per-frame) pair the
    // script system needs. SetScene is a no-op while the name is stable.
    fds::ParamScript_SetScene(sceneName);
    fds::ParamScript_Tick(float(Timer));

    const bool tabNow = Keyboard[ScTab] != 0;
    if (tabNow && !tabPrev_) {
        const bool switchingToFC = (View != &FC);
        if (switchingToFC) {
            // Capture the scripted cam's CURRENT pose into FC so you
            // always land where you were watching. Init-time capture
            // gets stale once Animate_Objects walks the camera spline.
            // Without this, FC could be at a default sentinel or a
            // stale init pose → frozen frame, feels stuck.
            FC.ISource = View->ISource;
            Matrix_Copy(FC.Mat, View->Mat);
            FC.IFOV    = View->IFOV;
            CalcPersp(&FC);
        }
        View = switchingToFC ? &FC : sc->CameraHead;
        std::fprintf(stderr, "[TAB %s] toggled -> View=%s\n",
                     sceneName, (View == &FC) ? "FC" : "scene");
    }
    tabPrev_ = tabNow;
}

void SceneDriver::clearFrame()
{
    parallel_memset(VPage, 0, PageSize);
    parallel_memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
}

void SceneDriver::waitBackspaceRelease()
{
    while (Keyboard[ScBackSpace] && !g_shouldQuit.load()) continue;
}

Scene *loadSceneAligned(const char *fldPath)
{
    Scene *sc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
    std::memset(sc, 0, sizeof(Scene));
    LoadFLD(sc, fldPath);
    return sc;
}

void tagSceneMaterials(Scene *sc, bool restrictToScene)
{
    for (Material *m = MatLib; m; m = m->Next) {
        if (restrictToScene && m->RelScene != sc) continue;
        if (!m->Txtr) continue;
        m->Flags |= Mat_RGBInterp;
        m->Txtr->Flags |= Txtr_Tiled;
    }
}
