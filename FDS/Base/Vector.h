
#ifndef REVIVAL_VECTOR_H
#define REVIVAL_VECTOR_H

// #define SIMDE_ENABLE_NATIVE_ALIASES

#include <stdio.h>
#include "simde/x86/sse.h"
#include "simde/x86/sse4.1.h"
#include <math.h>

#pragma pack(push, 1)

// [12 Bytes]
struct Vector
{
	float x = 0.0f, y = 0.0f, z = 0.0f;

	void Read(FILE* f) {
		fread(&x, 1, sizeof(float), f);
		fread(&y, 1, sizeof(float), f);
		fread(&z, 1, sizeof(float), f);
	}


	Vector() = default;
	Vector(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
	//~Vector() {}

	void print() {
		printf("(%f, %f, %f)", x, y, z);
	}

	bool is_zero() {
		return 0.0f == x && x == y && y == z;
	}

	inline float length()
	{
		return sqrtf(x * x + y * y + z * z);
	}

	inline Vector& normalize() {

		*this *= length();

		return *this;
	}


	/////////////////////////////////////////////////////////
	inline static void add(Vector& u, Vector& v, Vector& w)
	{
		w.x = u.x + v.x;
		w.y = u.y + v.y;
		w.z = u.z + v.z;
	}

	// this = u + v;
	inline void add(Vector& u, Vector& v)
	{
		x = u.x + v.x;
		y = u.y + v.y;
		z = u.z + v.z;
	}

	inline Vector operator + (const Vector& u) const
	{
		return Vector(x + u.x, y + u.y, z + u.z);
	}

	inline Vector& operator += (const Vector& u)
	{
		x += u.x;
		y += u.y;
		z += u.z;
		return *this;
	}

	/////////////////////////////////////////////////////////
	inline static void sub(Vector& u, Vector& v, Vector& w)
	{
		w.x = u.x - v.x;
		w.y = u.y - v.y;
		w.z = u.z - v.z;
	}

	// this = u - v;
	inline void sub(Vector& u, Vector& v)
	{
		x = u.x - v.x;
		y = u.y - v.y;
		z = u.z - v.z;
	}

	inline Vector operator - (const Vector& u) const
	{
		return Vector(x - u.x, y - u.y, z - u.z);
	}

	inline Vector& operator -= (const Vector& u)
	{
		x -= u.x;
		y -= u.y;
		z -= u.z;
		return *this;
	}

	/////////////////////////////////////////////////////////
	inline static void cross(const Vector& u, const Vector& v, Vector& w)
	{
		w.x = u.y * v.z - u.z * v.y;
		w.y = u.z * v.x - u.x * v.z;
		w.z = u.x * v.y - u.y * v.x;
	}

	inline void cross(const Vector& u, const Vector& v)
	{
		x = u.y * v.z - u.z * v.y;
		y = u.z * v.x - u.x * v.z;
		z = u.x * v.y - u.y * v.x;
	}

	Vector cross(const Vector& v) const {
		return Vector(
			y * v.z - z * v.y,
			z * v.x - x * v.z,
			x * v.y - y * v.x);
	}

	inline Vector operator ^ (const Vector& v) const
	{

		return Vector(
			y * v.z - z * v.y,
			z * v.x - x * v.z,
			x * v.y - y * v.x);
	}

	inline Vector& operator ^= (const Vector& v)
	{
		Vector	t(x, y, z);
		x = t.y * v.z - t.z * v.y;
		y = t.z * v.x - t.x * v.z;
		z = t.x * v.y - t.y * v.x;
		return *this;
	}
	/////////////////////////////////////////////////////////
	inline static float dot(Vector& u, Vector& v)
	{
		return (u.x * v.x + u.y * v.y + u.z * v.z);
	}

	inline float operator * (const Vector& u) const
	{
		return (u.x * x + u.y * y + u.z * z);
	}


	/////////////////////////////////////////////////////////
	Vector& operator *= (float s)
	{
		x *= s;
		y *= s;
		z *= s;
		return *this;
	}
	Vector& operator /= (float s)
	{
		float r = 1 / s;
		x *= r;
		y *= r;
		z *= r;
		return *this;
	}
	Vector operator * (float s) const
	{
		return Vector(x * s, y * s, z * s);
	}

	Vector operator / (float s) const
	{
		float r = 1 / s;
		return Vector(x * r, y * r, z * r);
	}

	friend Vector operator * (float s, Vector& v);
};

inline Vector operator * (float s, Vector& v)
{
	return Vector(v.x * s, v.y * s, v.z * s);
}

struct alignas(16) XMMVector
{

	using this_type = XMMVector;

	inline XMMVector() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
	//inline XMMVector(const __m128 &other) : v(other) {}
	inline XMMVector(float x, float y, float z) : x(x), y(y), z(z), w(0.0f) {}
	inline XMMVector(const Vector& v) : XMMVector(v.x, v.y, v.z) {}
	union // blah
	{
		__m128 v;
		struct {
			float x, y, z, w;
		};
	};


	inline this_type abs() const 
	{
		this_type result;
		result.v = _mm_andnot_ps(_mm_set1_ps(-0.), v);
		return result;
	}

	inline friend this_type max(const this_type &lhs, const this_type &rhs) 
	{
		this_type result;
		result.v = _mm_max_ps(lhs.v, rhs.v);
		return result;
	}

	inline friend this_type max(const this_type &lhs, float f) 
	{
		this_type rhs;
		rhs.v = _mm_set1_ps(f);
		return max(lhs, rhs);
	}

	// Returns Vector length of Vector V.
	inline __m128 InverseNorm()
	{
		return _mm_rsqrt_ps(_mm_dp_ps(v, v, 0x77));
	}

	inline float Length()
	{
		// (v dot v) * 1/sqrt(v dot v)
		if (x == 0.0 && y == 0.0 && z == 0) 
		{
			return 0.f;
		}
		auto dp = _mm_dp_ps(v, v, 0x71);
		return _mm_cvtss_f32(_mm_mul_ss(dp , _mm_rsqrt_ss(dp)));
	}

	inline void Normalize()
	{
		v = _mm_mul_ps(v, InverseNorm());
	}

	//this_type(this_type &&) = default;

	inline this_type& operator+=(const this_type& rhs)
	{
		v = _mm_add_ps(v, rhs.v);
		return *this;
	}

	inline friend this_type operator+(this_type lhs, const this_type& rhs)
	{
		lhs += rhs;
		return lhs;
	}

	inline this_type& operator-=(const this_type& rhs)
	{
		v = _mm_sub_ps(v, rhs.v);
		return *this;
	}

	inline friend this_type operator-(this_type lhs, const this_type& rhs)
	{
		lhs -= rhs;
		return lhs;
	}

	inline this_type& operator*=(float rhs)
	{
		const __m128 scalar = _mm_set1_ps(rhs);
		v = _mm_mul_ps(v, scalar);
		return *this;		
	}

	inline friend this_type operator*(this_type lhs, float rhs)
	{
		lhs *= rhs;
		return lhs;
	}

	inline this_type& operator*=(const this_type& rhs)
	{
		v = _mm_mul_ps(v, rhs.v);
		return *this;
	}

	inline friend this_type operator*(this_type lhs, const this_type& rhs)
	{
		lhs *= rhs;
		return lhs;
	}

	this_type cross(const this_type& v) const {
		return this_type(
			y * v.z - z * v.y,
			z * v.x - x * v.z,
			x * v.y - y * v.x);
	}

};


#pragma pack(pop)

#endif //REVIVAL_VECTOR_H
