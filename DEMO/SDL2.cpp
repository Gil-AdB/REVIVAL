#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <SDL.h>
#include <cstdio>

#ifdef __EMSCRIPTEN__
#include "../Modplayer/Modplayer.h"
#include <emscripten/threading.h>
#endif


static VESA_Surface SDL_MainSurf;
static SDL_Window *sdl_window;

static void V_Flip(VESA_Surface *VS)
{
	// Top-right resolution overlay. Draw into the surface's data buffer
	// just before pushing to the SDL_Texture so it shows up regardless of
	// which scene's surface is being flipped (MainSurf vs Glat's FinalSurf).
	if (VS->Data && VS->X > 0 && VS->Y > 0) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%dx%d", (int)VS->X, (int)VS->Y);
		// Approximate text width: ~10 px per char at the standard font.
		// Right-align at a small inset; if the surface is narrow we just
		// clamp to 0.
		int textPx = (int)strlen(buf) * 10;
		int x = (int)VS->X - textPx - 8;
		if (x < 0) x = 0;
		OutTextXY(VS->Data, x, 4, buf, 255, (int)VS->X, (int)VS->Y);
	}
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


void *SDL2_CreateChildTexture(int X, int Y)
{
	// Same renderer the engine display uses; assumes SDL2_InitDisplay ran.
	if (!SDL_MainSurf.Renderer || X <= 0 || Y <= 0) return nullptr;
	return SDL_CreateTexture(static_cast<SDL_Renderer *>(SDL_MainSurf.Renderer),
	                         SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
	                         X, Y);
}

// Tear down + reallocate the SDL-side framebuffer / Z-buffer / SDL_Texture
// at new dimensions, then re-install into the engine globals via
// VESA_VPageExternal (which calls Build_YOffs_Table + VESA_Surface2Global).
// Called from the demo thread at a frame boundary by EngineResize.
void SDL2_HandleResize(int newX, int newY)
{
	fprintf(stderr, "[RESIZE] requested %dx%d, current %dx%d\n",
	        newX, newY, SDL_MainSurf.X, SDL_MainSurf.Y);
	if (newX <= 0 || newY <= 0) { fprintf(stderr, "[RESIZE] skip: bad dims\n"); return; }
	if (newX == SDL_MainSurf.X && newY == SDL_MainSurf.Y) { fprintf(stderr, "[RESIZE] skip: same\n"); return; }
	fprintf(stderr, "[RESIZE] applying\n");

	// Free the engine-side YOffs that came from the previous
	// Build_YOffs_Table(MainSurf); VESA_VPageExternal stomps the pointer
	// via memcpy below, so freeing it here avoids the leak.
	if (MainSurf && MainSurf->YTable) {
		delete[] MainSurf->YTable;
		MainSurf->YTable = nullptr;
	}

	if (SDL_MainSurf.Handle) {
		SDL_DestroyTexture(static_cast<SDL_Texture *>(SDL_MainSurf.Handle));
		SDL_MainSurf.Handle = nullptr;
	}
	if (SDL_MainSurf.Data) {
		free(SDL_MainSurf.Data);
		SDL_MainSurf.Data = nullptr;
	}

	SDL_MainSurf.X = newX;
	SDL_MainSurf.Y = newY;
	V_Create(&SDL_MainSurf, static_cast<SDL_Renderer *>(SDL_MainSurf.Renderer));

	VESA_VPageExternal(&SDL_MainSurf);
}

dword SDL2_InitDisplay(SDL_Window *window)
{
	sdl_window = window;
	SDL_MainSurf.BPP = 32;

	// Fill in the secondary surface VSurf structure

	// Create a renderer with V-Sync enabled.
	// On Emscripten with PROXY_TO_PTHREAD, WebGL contexts can't be cleanly
	// created from a pthread worker (the canvas gets transferred to the
	// worker for offscreen rendering and becomes inaccessible to the main
	// thread's GL init path). Force the software renderer there — the
	// final frame is just an SDL_UpdateTexture + SDL_RenderCopy anyway, so
	// CPU vs WebGL for that last step is a minor perf detail.
#ifdef __EMSCRIPTEN__
	SDL_Renderer * renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
#else
	SDL_Renderer * renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_PRESENTVSYNC);
#endif
	SDL_MainSurf.Renderer = renderer;

	// Use renderer output size (in pixels) rather than window size (in
	// points). With SDL_WINDOW_ALLOW_HIGHDPI on a retina display these
	// differ by the DPI scale factor; we always want pixels because that's
	// what the framebuffer + SDL_Texture are sized in.
	int px, py;
	SDL_GetRendererOutputSize(renderer, &px, &py);
	SDL_MainSurf.X = px; SDL_MainSurf.Y = py;

	V_Create(&SDL_MainSurf, renderer);

	SDL_MainSurf.Flip = V_Flip;

	VESA_VPageExternal(&SDL_MainSurf);

	V_Flip(MainSurf);

	FPU_LPrecision();

	return 0;
}

#ifdef __EMSCRIPTEN__
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_cb_count = 0;

static void wasm_audio_callback(void* userdata, Uint8* stream, int len)
{
	if (g_audio_cb_count < 3) {
		fprintf(stderr, "[AUDIO] callback #%d len=%d ud=%p\n",
		        g_audio_cb_count, len, userdata);
	}
	g_audio_cb_count++;
	// 512 frames × 2 channels × sizeof(float) = 4096 bytes per callback.
	Modplayer_FillBuffer((ModplayerHandle)userdata,
	                     reinterpret_cast<float*>(stream),
	                     len / (2 * sizeof(float)));
}

// Runs on the browser main thread (proxied via emscripten_sync_run_in_main_runtime_thread).
// SDL2's emscripten audio implementation references a JS-side `SDL2` global
// that only exists in the main thread's JS context, so the open call must
// happen there.
static void open_audio_main_thread(void* modplayerHandle)
{
	SDL_AudioSpec want = {};
	SDL_AudioSpec have = {};
	want.freq = 48000;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = 512;  // matches xmplayer's AUDIO_BUF_FRAMES
	want.callback = wasm_audio_callback;
	want.userdata = modplayerHandle;
	g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	fprintf(stderr, "[AUDIO] OpenAudioDevice -> dev=%u (err=%s)\n",
	        (unsigned)g_audio_dev, g_audio_dev ? "ok" : SDL_GetError());
	if (g_audio_dev) {
		fprintf(stderr, "[AUDIO] have: freq=%d fmt=%04x ch=%d samples=%d\n",
		        have.freq, have.format, have.channels, have.samples);
		SDL_PauseAudioDevice(g_audio_dev, 0);
		fprintf(stderr, "[AUDIO] device unpaused\n");
	}
}

void SDL2_StartMusic(void* modplayerHandle)
{
	if (g_audio_dev || !modplayerHandle) return;
	emscripten_sync_run_in_main_runtime_thread(
		EM_FUNC_SIG_VI, &open_audio_main_thread, modplayerHandle);
}

void SDL2_StopMusic()
{
	if (!g_audio_dev) return;
	SDL_CloseAudioDevice(g_audio_dev);
	g_audio_dev = 0;
}
#endif

