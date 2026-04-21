#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <SDL.h>


static VESA_Surface SDL_MainSurf;
static SDL_Window *sdl_window;

static void V_Flip(VESA_Surface *VS)
{
	auto x = SDL_UpdateTexture(static_cast<SDL_Texture*>(VS->Handle), NULL, VS->Data, VS->BPSL);
	auto y = SDL_RenderCopy(static_cast<SDL_Renderer*>(VS->Renderer), static_cast<SDL_Texture*>(VS->Handle), NULL, NULL);
	SDL_RenderPresent(static_cast<SDL_Renderer*>(VS->Renderer));
}

static dword V_Create(VESA_Surface *VS, SDL_Renderer * renderer)
{
	VS->CPP = (VS->BPP+1)>>3;
	VS->BPSL = VS->CPP * VS->X;
	VS->PageSize = VS->BPSL * VS->Y;

	dword ZBufferSize = sizeof(word) * VS->X * VS->Y;
	if (!(VS->Data = (byte *)malloc(VS->PageSize + ZBufferSize))) return 1;
	memset(VS->Data,0,VS->PageSize + ZBufferSize);


	SDL_Texture * screen_texture = SDL_CreateTexture(renderer,
		SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
		VS->X, VS->Y);

	VS->Handle = static_cast<void *>(screen_texture);

	return 0;
}


dword SDL2_InitDisplay(SDL_Window *window)
{
	sdl_window = window;
	int x, y;
	SDL_GetWindowSize(sdl_window, &x, &y);
	SDL_MainSurf.X = x; SDL_MainSurf.Y = y;
	SDL_MainSurf.BPP = 32;

	// Fill in the secondary surface VSurf structure
	
	// Create a renderer with V-Sync enabled.
	SDL_Renderer * renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_PRESENTVSYNC);
	SDL_MainSurf.Renderer = renderer;

	V_Create(&SDL_MainSurf, renderer);

	SDL_MainSurf.Flip = V_Flip;

	VESA_VPageExternal(&SDL_MainSurf);

	V_Flip(MainSurf);

	FPU_LPrecision();
	
	return 0;
}

