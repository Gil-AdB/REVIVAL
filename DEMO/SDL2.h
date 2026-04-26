#ifndef _REV_SDL2_H_INCLUDED
#define _REV_SDL2_H_INCLUDED

#include <SDL.h>
#include "Base/FDS_VARS.H"

dword SDL2_InitDisplay(SDL_Window * window);
dword SDL2_RemoveDisplay();

void SDL2_Flip(VESA_Surface *VS);

#ifdef __EMSCRIPTEN__
// Emscripten music: the Rust modplayer-lib is built with the external-audio
// feature (no Rust-side audio backend), so the host opens an SDL_AudioDevice
// here and pulls samples via Modplayer_FillBuffer. The demo thread is gated
// behind first user input so this call happens after a gesture and the
// AudioContext can start.
void SDL2_StartMusic(void* modplayerHandle);
void SDL2_StopMusic();
#endif


#endif