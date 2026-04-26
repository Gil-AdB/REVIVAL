#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <SDL.h>

#ifdef __EMSCRIPTEN__
#include "../Modplayer/Modplayer.h"
#include <emscripten/threading.h>
#endif


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

