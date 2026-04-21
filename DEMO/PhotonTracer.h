#ifndef DEMO_PHOTONTRACER_H_INCLUDED
#define DEMO_PHOTONTRACER_H_INCLUDED

#include "Rev.h"

enum ptErrorCode
{
	// generic OK response
	PT_OK = 0, 
	// generic error response
	PT_ERROR = 1, 
	// insufficient system memory
	PT_NOMEMORY = 100, 
};

enum ptMaterial
{
	PT_MAT_UNDEFINED = 0,
	PT_MAT_GLASS = 1,
};

enum ptObjectFlags
{
	// light may travel across the interior of objects that have this flag set.
	PT_OBJ_TRANSCLUENT = 1 << 0,
	
};

typedef float ptRealType;

class ptVector2
{
public:
	ptRealType x, y;

	inline ptVector2() {}
	inline ~ptVector2() {}
	inline ptVector2(ptRealType _x, ptRealType _y): x(_x), y(_y) {}

	void fromPolar(ptRealType angle, ptRealType radius);

	inline friend ptVector2 operator + (const ptVector2 &u, const ptVector2 &v);
	inline friend ptVector2 operator - (const ptVector2 &u, const ptVector2 &v);
	inline friend ptRealType operator * (const ptVector2 &u, const ptVector2 &v);
	inline friend ptVector2 operator * (const ptVector2 &u, ptRealType s);
	inline friend ptVector2 operator * (ptRealType s, const ptVector2 &u);

	inline ptVector2 &operator += (const ptVector2 &u);
	inline ptVector2 &operator -= (const ptVector2 &u);
	inline ptVector2 &operator *= (const ptRealType s);

	void normalize();
};

// Overloaded operators for ptVector2
inline ptVector2 operator + (const ptVector2 &u, const ptVector2 &v) 
{
	return ptVector2(u.x + v.x, u.y + v.y);
}

inline ptVector2 operator - (const ptVector2 &u, const ptVector2 &v)
{
	return ptVector2(u.x - v.x, u.y - v.y);
}

inline ptRealType operator * (const ptVector2 &u, const ptVector2 &v)
{
	return u.x * v.x + u.y * v.y;
}

inline ptVector2 operator * (const ptVector2 &u, ptRealType s)
{
	return ptVector2(u.x * s, u.y * s);
}
inline ptVector2 operator * (ptRealType s, const ptVector2 &u)
{
	return ptVector2(s * u.x, s * u.y);
}

inline ptVector2 &ptVector2::operator += (const ptVector2 &u)
{
	x += u.x;
	y += u.y;
	return *this;
}

inline ptVector2 &ptVector2::operator -= (const ptVector2 &u)
{
	x -= u.x;
	y -= u.y;
	return *this;
}

inline ptVector2 &ptVector2::operator *= (const ptRealType s)
{
	x *= s;
	y *= s;
	return *this;
}

class ptVector3
{
public:
	ptVector3() {}
	ptVector3(ptRealType _x, ptRealType _y, ptRealType _z): x(_x), y(_y), z(_z) {}
	ptRealType x, y, z;
	inline friend ptVector3 operator * (const ptVector3 &u, const ptRealType s);
	inline friend ptVector3 operator * (const ptRealType s, const ptVector3 &u);
};

inline ptVector3 operator * (const ptVector3 &u, const ptRealType s)
{
	return ptVector3(u.x * s, u.y * s, u.z * s);
}
inline ptVector3 operator * (const ptRealType s, const ptVector3 &u)
{
	return ptVector3(s * u.x, s * u.y, s * u.z);
}



class ptObject2DVertex
{
public:
	ptVector2 _position;
	ptVector2 _normal; // this is the normal to the edge preceding the vertex at CCW order.
	ptRealType _lineOffset; // equals to <_normal, _position>
};

class ptObject2D
{
public:
	ptObject2D();
	~ptObject2D();
	ptErrorCode create(mword numVerts);
	
	ptObject2D(mword numVerts);
	void computeNormals();

	mword _flags;
	ptMaterial _material;
	
	mword _numVerts;
	ptObject2DVertex *_verts;

	inline ptObject2DVertex &operator[] (mword index) {return _verts[index];}
	inline const ptObject2DVertex &operator[] (mword index) const {return _verts[index];}
};

class ptPhoton2D
{
public:
	ptVector2 _position, _direction;
	ptRealType _waveLength;

	ptRealType _curRefIndex;

	ptPhoton2D(const ptVector2 &position, const ptVector2 &direction, ptRealType waveLength): 
		_position(position), _direction(direction), _waveLength(waveLength) {}
};

void TestPhotonTracer();

#endif //DEMO_PHOTONTRACER_H_INCLUDED