
#ifndef REVIVAL_SPLINE_H
#define REVIVAL_SPLINE_H

#include <stdio.h>
#include "BaseDefs.h"
#include "Quaternion.h"

#pragma pack(push, 1)

// [88 Bytes]
struct SplineKey
{
	void print() {
		printf("%f %f %f %f\n", Pos.x, Pos.y, Pos.z, Pos.W);
		printf("%f %f %f %f\n", AA.x, AA.y, AA.z, AA.W);
		printf("%f\n", Frame);

	}

	float Frame;                   // Key Frame Number

	Quaternion Pos;                // Normative Quaternion Rotation value
	Quaternion AA;                 // Angle-Axis Quaternion Rotation value

	// 5 Spline Attributive keys.
	float  Tens,                   // Key tension value
		Cont,                   // Key continuity value
		Bias,                   // Key bias value
		EaseTo,                 // Key ease to value
		EaseFrom;               // Key ease from value

	Quaternion DS, DD;             // Key Quaternionic Derivatives
};

// [16 Bytes]
// Total: [16 Bytes+88 Bytes per Key]
struct Spline
{
	DWord NumKeys = 0,CurKey = 0;
	SplineKey *Keys				= nullptr;
	DWord Flags					= 0;
	void print() {
		printf("Flags: %X\n", Flags);
		for (auto i = 0UL; i < NumKeys; ++i) {
			Keys[i].print();
			printf("\n");
		}
	}
};

#pragma pack(pop)
#endif //REVIVAL_SPLINE_H
