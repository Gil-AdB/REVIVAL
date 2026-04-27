#ifndef _FILLERTEST_H_INCLUDED_
#define _FILLERTEST_H_INCLUDED_

#include "Base/FDS_DECS.H"
#include "FILLERS/F4Vec.h"

#include <cstdint>

void FillerTest();

// Deterministic single-frame entry points for the snapshot harness. The
// pair of triangles share the V0–V2 edge — diffing the rendered VPage
// across native and wasm isolates rasterizer-edge / mask behaviour
// without the rest of the city pipeline in the way. `seed` selects a
// rotation; the same seed produces bit-identical input geometry.
void FillerTestSnapshotInit(int xres, int yres);
void FillerTestSnapshotRender(int32_t seed);
void FillerTestSnapshotCleanup();

#endif //_FILLERTEST_H_INCLUDED_