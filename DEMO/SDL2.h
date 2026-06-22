#ifndef _REV_SDL2_H_INCLUDED
#define _REV_SDL2_H_INCLUDED

#include <SDL.h>
#include <memory>
#include "Base/FDS_VARS.H"

// RAII for SDL_Texture. The deleter carries a short tag (e.g. "engine",
// "glat-final") so create/destroy show up in the trace identifying which
// logical texture they belong to. VESA_Surface::Handle stays as a non-owning
// void* — these RAII owners live in the .cpp that conceptually owns the
// texture, and Handle is just a view.
struct SDLTexDeleter {
    const char *tag = nullptr;
    void operator()(SDL_Texture *t) const noexcept;
};
using SDLTex = std::unique_ptr<SDL_Texture, SDLTexDeleter>;

// Allocates a streaming ARGB8888 SDL_Texture and prints a "[SDL] +tex" line.
// Returns an owning SDLTex; caller decides where to keep it alive. Pass a
// stable C-string literal for `tag` (it's stored in the deleter).
SDLTex SDL2_MakeTexture(SDL_Renderer *r, int X, int Y, const char *tag);

dword SDL2_InitDisplay(SDL_Window * window);
dword SDL2_RemoveDisplay();

void SDL2_Flip(VESA_Surface *VS);

// Allocate a dedicated streaming SDL_Texture sized to (X, Y) on the same
// renderer the main display uses. Intended for "child" VESA_Surfaces (e.g.
// Glat's FinalSurf) that flip to SDL but should NOT track engine-side
// window resizes — by holding their own texture, they survive the
// destroy/recreate dance in SDL2_HandleResize, and SDL_RenderCopy
// auto-stretches their content to whatever size the window has become.
SDLTex SDL2_CreateChildTexture(int X, int Y, const char *tag);

// Arms a fade-in on the engine surface: the next `frames` V_Flip calls
// will scale VPage in place by an increasing factor (1/N → 1.0) using
// the engine's AlphaBlend, so each scene fades up from black.
// Counterpart to engineFadeStep (SceneTick.h) which does fade-out.
extern "C" void EngineStartFadeIn(int frames);

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
