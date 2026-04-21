#ifndef REVIVAL_COLOR_H
#define REVIVAL_COLOR_H

#include "BaseDefs.h"

#pragma pack(push, 1)

// [16 Bytes]
struct Color
{
	float B = 0.0f, G = 0.0f, R = 0.0f, A = 0.0f;
};

// [4 Bytes]
union QColor
{
    dword BGRA = 0;
    struct
    {
        byte B,G,R,A; //Red,Green,Blue,Alpha.
    };
};

#pragma pack(pop)

#endif //REVIVAL_COLOR_H
