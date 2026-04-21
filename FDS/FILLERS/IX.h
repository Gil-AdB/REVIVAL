#ifndef IX_H_INCLUDED
#define IX_H_INCLUDED

#include "Base/FDS_DECS.H"
#include "F4Vec.h"

//#define MEASURE_ZSTATS
//#define MEASURE_POLYSTATS

struct IXVertexG
{
	union
	{
		struct
		{
			float B, G, R, z;	 // 16 bytes
		};
		F4Vec BGRZ;
	};	
	float x, RZ;		 // 8 bytes
	float y;			 // 4 bytes
	dword _align16[1];	 // 4 bytes
						 // Sum - 32 Bytes
};

struct IXVertexT
{
	union
	{
		struct
		{
			float x, RZ, UZ, VZ; // 16 bytes
		};
		F4Vec XZUV;
	};
	float y;			 // 4 bytes
	dword _align16[3];	 // 12 bytes
						 // Sum - 48 Bytes
};


struct alignas(16) IXVertexTG
{
	union
	{
		struct
		{
			float RZ, UZ, VZ, x; // 16 bytes
		};
		F4Vec ZUVX;
	};
	union
	{
		struct
		{
			float B, G, R, z;	 // 16 bytes
		};
		F4Vec BGRZ;
	};	
	float y;			 // 4 bytes
	dword _align16[3];	 // 12 bytes
						 // Sum - 48 Bytes
};

struct IXVertexF
{
	float R, G, B, z;	 // 16 bytes
	float x, RZ;		 // 8 bytes
	float y;			 // 4 bytes
	dword _align16[1];	 // 4 bytes
						 // Sum - 32 Bytes
};

extern mword zPass, zReject;
extern int64_t precisePixelCount;


// Interface to assembly mappers
extern "C"
{
	typedef void (*AsmFiller)(IXVertexTG* Verts, dword numVerts, void* Texture, void* Page, dword logWidth, dword logHeight);
};

extern thread_local AsmFiller p_IXTGZM_AsmFiller;
extern thread_local AsmFiller p_IXTGZSAM_AsmFiller;
extern thread_local AsmFiller p_IXTGZTAM_AsmFiller;
extern thread_local AsmFiller p_IXTGZTM_AsmFiller;
extern thread_local AsmFiller p_IXTGZTTAM_AsmFiller;


#endif //IX_H_INCLUDED
