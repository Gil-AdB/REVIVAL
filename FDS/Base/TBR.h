#ifndef REVIVAL_TBR_H
#define REVIVAL_TBR_H

#include "BaseDefs.h"
#include "Face.h"

#pragma pack(push, 1)

// entry for TBR - implements multiple lists of entries over a vector.
// renderZ encodes back-to-front sort key in view-space depth: for sprites
// it's the particle's view-space Z; for transparent face fragments it's
// the back-facing tri's max-Z (far surface) or the front-facing tri's
// min-Z (near surface). At flush time each per-strip linked list is
// sorted descending by renderZ so the per-pixel composite is back-to-front.
struct TBREntry
{
    Face *F;
    dword next;
    float renderZ;
    // Facing rank, PRECOMPUTED single-threaded at insert time: back(0),
    // sprite(1), front(2). The per-strip sort's tie-break and the clump
    // classifier consume THIS instead of re-deriving facing from the live
    // TPos SoA during the walk — re-deriving raced with concurrent strip
    // work (nondeterministic transparent order, ~1-in-8 frames: the greets
    // mirror screens flashed pink). Insert runs on the render thread before
    // any strip is dispatched, so the value is stable by construction.
    uint8_t rank;
};

#pragma pack(pop)

#endif //REVIVAL_TBR_H
