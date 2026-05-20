#ifndef _SKYCUBE_H_INCLUDED_
#define _SKYCUBE_H_INCLUDED_
#include "Base/FDS_DECS.H"

// Linear (row-major, ARGB8888) copy of one of the 6 sky-cube face
// textures, with a box-filtered mip pyramid down to 1×1. Captured in
// InitSkyCube *before* Sachletz tile-shuffles the originals, so the
// deferred-skybox pass can sample directly without reversing the
// cache-tile layout. face ∈ [0,5]. mip 0 is full resolution; higher
// mips are 2× smaller per side. Returns nullptr if InitSkyCube hasn't
// run for this scene. Out-params receive the requested mip's
// dimensions.
const dword *SkyCube_GetFaceMip(int face, int mip, int &outW, int &outH);

// Number of mip levels in a face (same for all faces — full-res ÷ 2
// until 1×1). Returns 0 if no sky cube installed.
int SkyCube_NumMips();

#endif //_SKYCUBE_H_INCLUDED_