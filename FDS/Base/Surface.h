#ifndef REVIVAL_SURFACE_H
#define REVIVAL_SURFACE_H

#include "BaseDefs.h"

#pragma pack(push, 1)

struct Surface
{
	DWord         * STex;  // Static Texture Map (64x64)
	DWord         * LMap;  // Light Map (16x16)
	DWord         * DTex;  // Dynamic Texture Map (64x64)
	DWord						TCacheEntry; //last frame surface was inserted to cache
	int32_t            TCacheOffset; //-1 if surface is cached ; Cache pointer index otherwise.
};

#pragma pack(pop)

#endif //REVIVAL_SURFACE_H
