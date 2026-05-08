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
	PROF_SKY	=	1,  // RenderSkyCube — was inflating ZCLR in city/fountain
	PROF_ANIM	=	2,
	PROF_XFRM	=	3,
	PROF_LGHT   =	4,
	PROF_SORT	=	5,
	PROF_RNDR	=	6,
	PROF_FLIP	=	7,
	PROF_NUM	=	8
};

extern ModplayerHandle g_RevModuleHandle;
extern dword g_profilerActive;

void Destroy_Scene(Scene *Sc);

void Run_Glato(void);

void Initialize_Credits();
void Run_Credits();

void Initialize_Glato();
void Initialize_City();
void Run_City();

void Initialize_Chase();
void Run_Chase();

void Initialize_Fountain();
void Run_Fountain();

void Initialize_Greets();
void Run_Greets();

void Initialize_Crash();
void Run_Crash();

#endif