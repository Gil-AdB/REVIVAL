#ifndef REVIVAL_OBJECT_H
#define REVIVAL_OBJECT_H

#include "BaseDefs.h"
#include "Vector.h"
#include "Matrix.h"

#pragma pack(push, 1)

// [16 Bytes per Status Key]
struct ObjectStatus
{
    float          Frame	= 0.0f;
    DWord          Stat		= 0;
    ObjectStatus * Next		= nullptr;
    ObjectStatus * Prev		= nullptr;
};

struct Material;

// [36 Bytes]
struct  Object
{
	void           * Data		= nullptr;
	DWord            Type		= 0;
	DWord            Number		= 0;
	Vector         * Pos		= nullptr;
	Matrix         * Rot		= nullptr;
	Vector           Pivot;
	Object		   * Next		= nullptr;
	Object		   * Prev		= nullptr;
	Object		   * Parent		= nullptr;
	signed short	 ParentID	= 0;
	char           * Name		= nullptr;
	Material	   * Reflection = nullptr;
};

#pragma pack(pop)

#endif //REVIVAL_OBJECT_H
