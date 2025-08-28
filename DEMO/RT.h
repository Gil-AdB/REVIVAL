#include "Base/BaseDefs.h"
#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"
#include "Base/Object.h"
#include "Base/Vector.h"
#include "Rev.h"
#include "IMGGENR/IMGGENR.H"
#include "VESA/Vesa.h"

#include <_types/_uint32_t.h>
#include <algorithm>

	// tables, generated and used as an optimization.
#define TRIG_ACC 4096 //trigonometric table accuracy. must be a power of 2.
#define TRIG_MASK (TRIG_ACC-1)
#define TRIG_FACTOR (float(TRIG_ACC)/PI_M2)

#ifdef _C_WATCOM
#define VSurface Screen
#else
#define VSurface MainSurf
#endif


static int32_t InitScreenXRes, InitScreenYRes;

inline float sdBox(const XMMVector &p, const XMMVector &b)
{
  auto q = p.abs() - b;
  auto maxq = max(q, 0.0);
  auto maxqlen = maxq.Length();
  auto boxtest = std::min(std::max(q.x,std::max(q.y,q.z)),0.0f);
  return maxqlen + boxtest;
}

inline float sdSphere(const XMMVector &p, const XMMVector &c, float r)
{
	return (p - c).Length() - r;
}

// polynomial smooth min 2 (k=0.1)
float smin( float a, float b, float k )
{
    float h = std::max( k-abs(a-b), 0.0f )/k;
    return std::min( a, b ) - h*h*k*(1.0f / 4.0f);
}

inline float sdMeatBalls(const XMMVector &p, const XMMVector &s1, const XMMVector &s2)
{
	return smin(sdSphere(p, s1, 4.0f), sdSphere(p, s2, 4.0f), 0.1f);
}

class RT
{
	int32_t xres = XRes;
	int32_t yres = YRes;
	byte *Page1;
	VESA_Surface Surf1;
	uint32_t numFrames = 0;
	int32_t timerStack[20], timerIndex = 0;

	enum class OBJ
	{
		NONE,
		BOX,
		BACKGROUND,
		FLOOR,
	};

public:
	RT()
	{
		Page1 = (byte*)_aligned_malloc(PageSize, 32);
		memset(Page1, 0, PageSize);	
		memcpy(&Surf1,VSurface,sizeof(VESA_Surface));
		Surf1.Data = Page1;
		Surf1.Flags = VSurf_Noalloc;
		Surf1.Targ = NULL;
		for(int i=0; i<20; i++) {
			timerStack[i] = Timer;
		}
	}

	std::pair<float, OBJ> map(XMMVector p, XMMVector rd)
	{
		const float background_z = 50.0f;
		const float floor_y = 0.f;
		XMMVector box{3.0, 3.0, 3.0};

		auto mat = OBJ::BOX;
		//auto d = sdBox(p, box);
		float x1 = 5.0 + 2.0 * sin(numFrames * 0.03);
		float y1 = 3.0 + 2.0 * sin(numFrames * 0.03 + 0.2);
		float z1 = 6.0 + 2.0 * sin(numFrames * 0.03 + 0.4);
		float x2 =-5.0 + 2.0 * sin(numFrames * 0.05 + 0.3);
		float y2 = 2.5 + 2.0 * sin(numFrames * 0.05 + 0.5);
		float z2 = 5.5 + 2.0 * sin(numFrames * 0.05 + 0.7);
		auto d = sdMeatBalls(p, {x1, y1, z1}, {x2, y2, z2});

		// distance to background
		auto dist_to_bg = fabs((background_z - p.z));
		if (dist_to_bg < d) {
			d = dist_to_bg;
			mat = OBJ::BACKGROUND;
		}

		// // distance to floor
		// auto dist_to_floor = (p.y - floor_y);
		// if (dist_to_floor > 0.f && dist_to_floor < d) {
		// 	d = dist_to_floor;
		// 	mat = OBJ::FLOOR;
		// }
		return std::make_pair(d, mat);
	}

	void RunFrame(float t)
	{
		XMMVector Intersection, Origin, Direction, U;
		XMMVector CameraPos(10, -5, -10.0);
		XMMMatrix CamMat;
		Matrix_Identity(CamMat.Data);
		const int MAX_ITERATIONS = 60;
		const float epsilon = 0.01;
		char MSGStr[128];
		
		memset(Surf1.Data,0,PageSize + XRes*YRes*sizeof(word));

		for (int y=0;y<yres;y++)
		{
			for (int x=0;x<xres;x++)
			{
				Direction.x = float((x - (xres >> 1)));
				Direction.y = float((y - (yres >> 1)));
				Direction.z = 256.f;
				Direction.w = .0f;
				
				// Direction = CamMat * Direction;

				Direction.Normalize();

				XMMVector p = CameraPos;

				int ii;
				for (ii = 0; ii < MAX_ITERATIONS; ++ii)
				{

					auto d = map(p, Direction);
					if (d.first < epsilon) // hit 
					{
						if (d.second == OBJ::BACKGROUND)
						{
							*((dword *)(&Surf1.Data[x * 4 + Surf1.BPSL * y])) = ((((int(p.x)) ^ (int(p.y))) >> 4) & 1) * 0xfffffff; 
						}
						else 
						{					
							*((dword *)(&Surf1.Data[x * 4 + Surf1.BPSL * y])) = uint32_t(d.second) * 0x424202;
						}
						break;
					}

					p += Direction * d.first;
				}
				// *((dword *)(&Surf1.Data[x * 4 + Surf1.BPSL * y])) = (128 - ii) * 0x020202;
			}
		}

		timerStack[timerIndex++] = Timer;
		if (timerIndex == 20)
		{
			timerIndex = 0;
			snprintf(MSGStr, sizeof(MSGStr), "%f FPS", 2000.0 / (float)(timerStack[19] - timerStack[timerIndex]));
		}
		else {
			snprintf(MSGStr, sizeof(MSGStr), "%f FPS", 2000.0 / (float)(timerStack[timerIndex - 1] - timerStack[timerIndex]));
		}
		auto scroll = OutTextXY(Surf1.Data, 0, 0, MSGStr, 255);
		snprintf(MSGStr, sizeof(MSGStr), "Frame: %d", numFrames);
		scroll = OutTextXY(Surf1.Data, 0, scroll + 15, MSGStr, 255);
		Flip(&Surf1);
		numFrames++;
	}
};

