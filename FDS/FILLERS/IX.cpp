// House IX technology
// initial prototypes for advanced texture mappers are placed here.
// raster function naming convension:
// IX_Prefiller_[ATTR]
// [ATTR] is composed of one or more of the following characters.
// T + texture mapper.
//  s - arbitrary size support (optional)
//  p - palettized texture support
//  b - block tiling support
// G - gouraud shading support
// F - flat shading support (automatically assumed if 'T' and 'G' are not set)
// A + alpha blending
//  c - constant transparent
//  g - gouraud alpha transparent
//  t - texture alpha transparent
// M + machine code (optional)
//  m - MMX instructions
//  s - SSE instructions
//  3 - pentium III instructions (implied automatically if 's' is set)
//  4 - pentium 4 instructions
// W - wireframe mode available
// Z + zbuffering support (optional)
//  w - zbuffer writeback can be disabled
//  c - zbuffer compares can be disabled

// example. our current mappers are called
// IX_Prefiller_TGAcZ()
// IX_Prefiller_TGZ()
// new flat mapper is called
// IX_Prefiller_FZ()

// internal vertex structs are called using the relevent subset of the above flags.
// e.g. vertex for IX_Prefiller_TGAcZ is IXVertexTG

#include "IX.h"

mword zPass, zReject;
int64_t precisePixelCount;
