#pragma once
typedef void *ModplayerHandle;

extern "C" {
	ModplayerHandle Modplayer_Create(const char* path);
	void Modplayer_Start(ModplayerHandle handle);
	void Modplayer_Stop(ModplayerHandle handle);
	void Modplayer_SetOrder(ModplayerHandle handle, unsigned int order);

#if defined(__EMSCRIPTEN__)
	// external-audio backend: host opens its own audio device (SDL_AudioDevice
	// in our case) and pulls samples via this entry point. `frames` must be
	// AUDIO_BUF_FRAMES (512) and `out` receives `frames * 2` interleaved
	// stereo f32s.
	void Modplayer_FillBuffer(ModplayerHandle handle, float* out, unsigned int frames);
#endif
}
