#ifndef _REV_SDL2_H_INCLUDED
#define _REV_SDL2_H_INCLUDED

#include <SDL.h>
#include "Base/FDS_VARS.H"

dword SDL2_InitDisplay(SDL_Window * window);
dword SDL2_RemoveDisplay();

void SDL2_Flip(VESA_Surface *VS);

// Allocate a dedicated streaming SDL_Texture sized to (X, Y) on the same
// renderer the main display uses. Intended for "child" VESA_Surfaces (e.g.
// Glat's FinalSurf) that flip to SDL but should NOT track engine-side
// window resizes — by holding their own texture, they survive the
// destroy/recreate dance in SDL2_HandleResize, and SDL_RenderCopy
// auto-stretches their content to whatever size the window has become.
void *SDL2_CreateChildTexture(int X, int Y);

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