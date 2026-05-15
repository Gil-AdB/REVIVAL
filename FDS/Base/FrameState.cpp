// Singleton render-context instances for the main pass, plus the
// legacy-name reference aliases that older code reads/writes through.
//
// Two phases of state evolution coexist here while phase 2 of the
// re-entrant refactor is in flight:
//
//   - fds::g_mainCamera + fds::g_mainFaces (new shape — what new code
//     and the shadow orchestrator will eventually pass explicitly).
//   - fds::g_mainFrame (legacy aggregate; deprecated, retained as a
//     small wrapper so existing FrameState* parameters still resolve).
//
// All three live in the same TU so their static-initialization order is
// deterministic — the alias references at the bottom of this file bind
// to fields inside g_mainCamera / g_mainFaces (not g_mainFrame), which
// is what makes the new context types the true source of truth.

#include "FrameState.h"
#include "CameraContext.h"
#include "FaceListContext.h"
#include "RenderTarget.h"
#include "FDS_VARS.H"
#include "FDS_DECS.H"
#include "FILLERS/Mekalele.h"   // g_gbuffer / g_gbufferTransparent / g_xparZ

namespace fds {

CameraContext   g_mainCamera;
FaceListContext g_mainFaces;

// Legacy struct kept while phase 2 lands. Its fields are no longer the
// canonical home for the per-frame state — they're transitional and
// will be removed once every site moves to CameraContext / FaceListContext.
FrameState g_mainFrame;

RenderTarget MainRenderTargetFromGlobals() {
    RenderTarget rt;
    rt.vpage              = reinterpret_cast<uint32_t*>(VPage);
    rt.bytesPerScanline   = VESA_BPSL;
    rt.zpage16            = ZPage16;
    rt.xres               = XRes;
    rt.yres               = YRes;
    rt.gbuffer                = g_gbuffer;
    rt.gbufferTransparent     = g_gbufferTransparent;
    rt.gbufferTransparentBack = g_gbufferTransparentBack;
    rt.xparZ                  = g_xparZ;
    rt.xparZBack              = g_xparZBack;
    return rt;
}

} // namespace fds

// Legacy-name reference aliases. Bind to the new context singletons so
// existing C-style globals (CAll, FList, FOVX, …) and the new struct
// fields share the same memory. Migration is gradual: each call site
// that takes CameraContext& / FaceListContext& reads/writes through the
// struct; sites still using the bare names hit the same bytes through
// the alias.
int32_t   &CPolys  = fds::g_mainFaces.cPolys;
int32_t   &COmnies = fds::g_mainFaces.cOmnies;
int32_t   &CPcls   = fds::g_mainFaces.cPcls;
int32_t   &CAll    = fds::g_mainFaces.cAll;
int32_t   &Polys   = fds::g_mainFaces.polys;
Face   ** &FList   = fds::g_mainFaces.fList;
Face   ** &SList   = fds::g_mainFaces.sList;

Camera *&View      = fds::g_mainCamera.view;
float     &FOVX    = fds::g_mainCamera.fovX;
float     &FOVY    = fds::g_mainCamera.fovY;
extern "C" {
    int32_t &CntrX = fds::g_mainCamera.cntrX;
    int32_t &CntrY = fds::g_mainCamera.cntrY;
}
float     &CntrEX  = fds::g_mainCamera.cntrEX;
float     &CntrEY  = fds::g_mainCamera.cntrEY;
float     &C_FZP   = fds::g_mainCamera.farZ;
float     &C_rFZP  = fds::g_mainCamera.invFarZ;
float     &C_NZP   = fds::g_mainCamera.nearZ;
float     &C_rNZP  = fds::g_mainCamera.invNearZ;

extern "C" {
    float &g_zscale    = fds::g_mainCamera.zScale;
    float &g_zscale256 = fds::g_mainCamera.zScale256;
}

// Were defined in the now-deleted FDS/FILLERS/IX.cpp; the legacy IX
// fillers wrote to them as ROP stats. Surviving callers (GREETS) just
// zero them; precisePixelCount is referenced by a declaration only.
dword   zReject = 0, zPass = 0;
int64_t precisePixelCount = 0;

