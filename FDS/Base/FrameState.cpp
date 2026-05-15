// Definition of g_mainFrame + the legacy-name reference aliases.
//
// Putting both in the same TU pins their static-initialization order:
// g_mainFrame is constructed first (POD with =0 defaults), then each
// reference binds to a field address in it.

#include "FrameState.h"
#include "FDS_VARS.H"

namespace fds {
FrameState g_mainFrame;
} // namespace fds

// Legacy-name reference aliases for backwards compatibility. Old code
// referring to `CAll`, `FList`, etc. transparently reads/writes the
// corresponding field of fds::g_mainFrame. As call sites migrate to
// take FrameState& explicitly, these stay in place — they're free at
// runtime (a reference is a compile-time alias on the same memory).
int32_t  &CPolys  = fds::g_mainFrame.CPolys;
int32_t  &COmnies = fds::g_mainFrame.COmnies;
int32_t  &CPcls   = fds::g_mainFrame.CPcls;
int32_t  &CAll    = fds::g_mainFrame.CAll;
int32_t  &Polys   = fds::g_mainFrame.Polys;
Face   ** &FList  = fds::g_mainFrame.FList;
Face   ** &SList  = fds::g_mainFrame.SList;
float     &FOVX   = fds::g_mainFrame.FOVX;
float     &FOVY   = fds::g_mainFrame.FOVY;
extern "C" {
    int32_t &CntrX  = fds::g_mainFrame.CntrX;
    int32_t &CntrY  = fds::g_mainFrame.CntrY;
}
float     &CntrEX = fds::g_mainFrame.CntrEX;
float     &CntrEY = fds::g_mainFrame.CntrEY;
float     &C_FZP  = fds::g_mainFrame.C_FZP;
float     &C_rFZP = fds::g_mainFrame.C_rFZP;
float     &C_NZP  = fds::g_mainFrame.C_NZP;
float     &C_rNZP = fds::g_mainFrame.C_rNZP;

extern "C" {
    float &g_zscale    = fds::g_mainFrame.g_zscale;
    float &g_zscale256 = fds::g_mainFrame.g_zscale256;
}

// Were defined in the now-deleted FDS/FILLERS/IX.cpp; the legacy IX
// fillers wrote to them as ROP stats. Surviving callers (GREETS) just
// zero them; precisePixelCount is referenced by a declaration only.
dword   zReject = 0, zPass = 0;
int64_t precisePixelCount = 0;
