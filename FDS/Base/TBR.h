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
};

#pragma pack(pop)

#endif //REVIVAL_TBR_H
