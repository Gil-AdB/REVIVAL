#ifndef _REV_SDL2_H_INCLUDED
#define _REV_SDL2_H_INCLUDED

#include <SDL.h>
#include "Base/FDS_VARS.H"

dword SDL2_InitDisplay(SDL_Window * window);
dword SDL2_RemoveDisplay();

void SDL2_Flip(VESA_Surface *VS);


#endif