#ifndef REVIVAL_MATRIX_H
#define REVIVAL_MATRIX_H

// [36 Bytes]
typedef float Matrix[3][3];
typedef float Matrix4[4][4]; //stuff

struct alignas(16) XMMMatrix {
	float Data[3][3] = {0};


	XMMVector operator*(XMMVector &U) const {
		XMMVector result;
		auto M = Data;

		result.x = M[0][0] * U.x + M[0][1] * U.y + M[0][2] * U.z;
		result.y = M[1][0] * U.x + M[1][1] * U.y + M[1][2] * U.z;
		result.z = M[2][0] * U.x + M[2][1] * U.y + M[2][2] * U.z;

		return result;
	}

};
struct alignas(16) XMMMatrix4 { float XMMMatrix4[4][4]; };


#endif //REVIVAL_MATRIX_H
