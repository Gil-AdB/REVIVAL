#ifndef DEMO_H_INC
#define DEMO_H_INC

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Raytracer.h"
#include <Base/Scene.h>
#include "../Modplayer/Modplayer.h"

enum
{
	PROF_ZCLR	=	0,
	PROF_ANIM	=	1,
	PROF_XFRM	=	2,
	PROF_LGHT   =	3,
	PROF_SORT	=	4,
	PROF_RNDR	=	5,
	PROF_FLIP	=	6,
	PROF_NUM	=	7
};

extern ModplayerHandle g_RevModuleHandle;
extern dword g_profilerActive;

void FlipRequest(VESA_Surface *VS);
void SysSleep(dword ticks);

void Destroy_Scene(Scene *Sc);

void Run_Glato(void);

void Initialize_Credits();
void Run_Credits();

void Initialize_Glato();
void Initialize_City();
static void TextureBlockTest();
void Run_City();

void Initialize_Chase();
void Run_Chase();

void Initialize_Fountain();
void Run_Fountain();

void Initialize_Greets();
void Run_Greets();

void Initialize_Crash();
void Run_Crash();

void Initialize_Koch();
void Run_Koch();

void Initialize_Nova();
void Run_Nova();

#endif