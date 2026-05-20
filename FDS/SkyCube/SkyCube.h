#ifndef _SKYCUBE_H_INCLUDED_
#define _SKYCUBE_H_INCLUDED_
#include "Base/FDS_DECS.H"

// Linear (row-major, ARGB8888) copy of one of the 6 sky-cube face
// textures. Captured in InitSkyCube *before* Sachletz tile-shuffles
// the originals, so the deferred-skybox pass can sample directly
// without reversing the cache-tile layout. face ∈ [0,5] per the
// SkyCube.cpp normal/order convention. Returns nullptr if InitSkyCube
// hasn't run for this scene (no sky cube installed).
const dword *SkyCube_GetFaceLinear(int face, int &outW, int &outH);

#endif //_SKYCUBE_H_INCLUDED_